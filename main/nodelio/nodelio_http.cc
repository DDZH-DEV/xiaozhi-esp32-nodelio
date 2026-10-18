#include "nodelio_http.h"

#include "nodelio_bridge.h"
#include "wifi_provision.h"

#include <arpa/inet.h>
#include <errno.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <esp_wifi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <fcntl.h>
#include <netdb.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#define TAG "nl_http"

#ifndef NOD_HTTP_CONNECT_MS
#define NOD_HTTP_CONNECT_MS 2000
#endif
#ifndef NOD_HTTP_IO_MS
#define NOD_HTTP_IO_MS 2500
#endif

/**
 * 息屏断网后 esp_wifi_stop，再 getaddrinfo → lwIP assert Invalid mbox。
 * 所有短 TCP 入口先拦，并与 wifi_sleep 互斥。
 */
static bool http_net_ready(void)
{
    if (nodelio_wifi_is_asleep()) {
        ESP_LOGW(TAG, "skip: wifi asleep");
        return false;
    }
    if (!nodelio_wifi_has_ip()) {
        ESP_LOGW(TAG, "skip: no IP");
        return false;
    }
    return true;
}

struct HttpNetGuard {
    bool ok = false;
    explicit HttpNetGuard(uint32_t lock_ms = 2000)
    {
        if (!http_net_ready()) {
            return;
        }
        if (!nodelio_wifi_net_acquire(lock_ms)) {
            ESP_LOGW(TAG, "skip: net lock busy (waited %ums)", (unsigned)lock_ms);
            return;
        }
        if (!http_net_ready()) {
            nodelio_wifi_net_release();
            return;
        }
        ok = true;
    }
    ~HttpNetGuard()
    {
        if (ok) {
            nodelio_wifi_net_release();
        }
    }
    explicit operator bool() const
    {
        return ok;
    }
    HttpNetGuard(const HttpNetGuard &) = delete;
    HttpNetGuard &operator=(const HttpNetGuard &) = delete;
};

/** 对话期小智已 PERFORMANCE；再 esp_wifi_set_ps 易与 coexist 死锁 → IWDT 重启 */
static void force_wifi_perf(void)
{
    if (nodelio_voice_session_active() || nodelio_wifi_is_asleep()) {
        return;
    }
    wifi_ps_type_t cur = WIFI_PS_MAX_MODEM;
    if (esp_wifi_get_ps(&cur) == ESP_OK && cur == WIFI_PS_NONE) {
        return;
    }
    (void)esp_wifi_set_ps(WIFI_PS_NONE);
}

static bool parse_host_port(const char *url, std::string *host, int *port, std::string *path, bool *https)
{
    if (!url || !host || !port || !path || !https) {
        return false;
    }
    const char *p = strstr(url, "://");
    if (!p) {
        return false;
    }
    *https = (strncmp(url, "https", 5) == 0);
    p += 3;
    const char *slash = strchr(p, '/');
    std::string authority = slash ? std::string(p, (size_t)(slash - p)) : std::string(p);
    *path = slash ? std::string(slash) : std::string("/");
    size_t colon = authority.find(':');
    if (colon != std::string::npos) {
        *host = authority.substr(0, colon);
        *port = atoi(authority.c_str() + colon + 1);
    } else {
        *host = authority;
        *port = *https ? 443 : 80;
    }
    return !host->empty() && *port > 0;
}

/** 短超时 TCP（勿用小智 EspTcp：其 connect 无超时，SYN 可卡数十秒拖垮音频）。io_ms 为读写超时。 */
static int tcp_connect_ms(const char *host, int port, int timeout_ms, int io_ms)
{
    /* 调用方须已持 HttpNetGuard；此处再验，防 sleep 竞态 */
    if (!http_net_ready()) {
        return -1;
    }
    struct addrinfo hints = {};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    char port_str[8];
    snprintf(port_str, sizeof(port_str), "%d", port);
    struct addrinfo *res = nullptr;
    int gai = getaddrinfo(host, port_str, &hints, &res);
    if (gai != 0 || !res) {
        ESP_LOGW(TAG, "getaddrinfo %s:%d fail %d", host, port, gai);
        return -1;
    }

    int fd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (fd < 0) {
        freeaddrinfo(res);
        return -1;
    }

    int flags = fcntl(fd, F_GETFL, 0);
    if (flags >= 0) {
        fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    }

    int cr = connect(fd, res->ai_addr, res->ai_addrlen);
    freeaddrinfo(res);
    if (cr == 0) {
        /* connected immediately */
    } else if (errno != EINPROGRESS) {
        ESP_LOGW(TAG, "connect %s:%d errno=%d", host, port, errno);
        close(fd);
        return -1;
    } else {
        fd_set wfds;
        FD_ZERO(&wfds);
        FD_SET(fd, &wfds);
        struct timeval tv;
        tv.tv_sec = timeout_ms / 1000;
        tv.tv_usec = (timeout_ms % 1000) * 1000;
        int sel = select(fd + 1, nullptr, &wfds, nullptr, &tv);
        if (sel <= 0) {
            ESP_LOGW(TAG, "connect %s:%d timeout %dms (sel=%d)", host, port, timeout_ms, sel);
            close(fd);
            return -1;
        }
        int so_err = 0;
        socklen_t sl = sizeof(so_err);
        if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &so_err, &sl) < 0 || so_err != 0) {
            ESP_LOGW(TAG, "connect %s:%d so_err=%d", host, port, so_err);
            close(fd);
            return -1;
        }
    }

    if (flags >= 0) {
        fcntl(fd, F_SETFL, flags);
    }
    if (io_ms <= 0) {
        io_ms = NOD_HTTP_IO_MS;
    }
    struct timeval rcv = {.tv_sec = io_ms / 1000, .tv_usec = (io_ms % 1000) * 1000};
    struct timeval snd = rcv;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &rcv, sizeof(rcv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &snd, sizeof(snd));
    return fd;
}

esp_err_t nodelio_http_probe_tcp(const char *host, int port)
{
    if (!host || !host[0] || port <= 0) {
        return ESP_ERR_INVALID_ARG;
    }
    HttpNetGuard net;
    if (!net) {
        return ESP_ERR_INVALID_STATE;
    }
    force_wifi_perf();
    ESP_LOGI(TAG, "TCP probe → %s:%d (%dms)…", host, port, NOD_HTTP_CONNECT_MS);
    int fd = tcp_connect_ms(host, port, NOD_HTTP_CONNECT_MS, NOD_HTTP_IO_MS);
    if (fd < 0) {
        ESP_LOGW(TAG, "TCP probe %s:%d FAIL", host, port);
        return ESP_FAIL;
    }
    close(fd);
    ESP_LOGI(TAG, "TCP probe %s:%d OK", host, port);
    return ESP_OK;
}

static bool send_all(int fd, const char *data, size_t len)
{
    size_t off = 0;
    while (off < len) {
        int n = send(fd, data + off, len - off, 0);
        if (n <= 0) {
            return false;
        }
        off += (size_t)n;
    }
    return true;
}

/** Gin/net/http 常发 Transfer-Encoding: chunked；不解包则 body 以 hex 尺寸开头，cJSON 必失败。 */
static bool header_has_chunked(const char *headers, size_t hdr_len)
{
    if (!headers || hdr_len == 0) {
        return false;
    }
    std::string h(headers, hdr_len);
    for (char &c : h) {
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c - 'A' + 'a');
        }
    }
    return h.find("transfer-encoding:") != std::string::npos && h.find("chunked") != std::string::npos;
}

static bool decode_chunked(const char *in, size_t in_len, std::string *out)
{
    if (!in || !out) {
        return false;
    }
    out->clear();
    size_t off = 0;
    while (off < in_len) {
        size_t line_end = off;
        while (line_end + 1 < in_len && !(in[line_end] == '\r' && in[line_end + 1] == '\n')) {
            line_end++;
        }
        if (line_end + 1 >= in_len) {
            return false;
        }
        unsigned long chunk = 0;
        for (size_t i = off; i < line_end; i++) {
            char c = in[i];
            if (c == ';' || c == ' ') {
                break;
            }
            unsigned v;
            if (c >= '0' && c <= '9') {
                v = static_cast<unsigned>(c - '0');
            } else if (c >= 'a' && c <= 'f') {
                v = static_cast<unsigned>(c - 'a' + 10);
            } else if (c >= 'A' && c <= 'F') {
                v = static_cast<unsigned>(c - 'A' + 10);
            } else {
                return false;
            }
            chunk = (chunk << 4) | v;
        }
        off = line_end + 2;
        if (chunk == 0) {
            return true;
        }
        if (off + chunk + 2 > in_len) {
            return false;
        }
        out->append(in + off, chunk);
        off += chunk;
        if (in[off] != '\r' || in[off + 1] != '\n') {
            return false;
        }
        off += 2;
    }
    return false;
}

/**
 * 服务端按「请求到达」做 400ms 限流。开机 TCP 握手可能要几百毫秒，
 * 若在 connect 之前记时间，下一发会以为已经隔够，到服务器仍落在窗口内。
 * 锁持有到 send 完成，时间戳记在发出之后。
 */
static portMUX_TYPE s_pace_init_mux = portMUX_INITIALIZER_UNLOCKED;
static StaticSemaphore_t s_pace_buf;
static SemaphoreHandle_t s_pace_mu;
static int64_t s_pace_sent_us;

static SemaphoreHandle_t skill_pace_mu(void)
{
    if (s_pace_mu) {
        return s_pace_mu;
    }
    portENTER_CRITICAL(&s_pace_init_mux);
    if (!s_pace_mu) {
        s_pace_mu = xSemaphoreCreateMutexStatic(&s_pace_buf);
    }
    portEXIT_CRITICAL(&s_pace_init_mux);
    return s_pace_mu;
}

struct SkillInvokePace {
    SemaphoreHandle_t mu = nullptr;

    explicit SkillInvokePace(const char *url)
    {
        if (!url || !strstr(url, "/panel/skills/") || !strstr(url, "/invoke")) {
            return;
        }
        mu = skill_pace_mu();
        if (!mu || xSemaphoreTake(mu, pdMS_TO_TICKS(8000)) != pdTRUE) {
            mu = nullptr;
            return;
        }
        const int64_t now = esp_timer_get_time();
        const int64_t wait_us = 450000 - (now - s_pace_sent_us);
        if (s_pace_sent_us != 0 && wait_us > 0 && wait_us < 3000000) {
            vTaskDelay(pdMS_TO_TICKS((wait_us + 999) / 1000));
        }
    }

    void mark_sent(void)
    {
        if (!mu) {
            return;
        }
        s_pace_sent_us = esp_timer_get_time();
        xSemaphoreGive(mu);
        mu = nullptr;
    }

    ~SkillInvokePace()
    {
        if (!mu) {
            return;
        }
        s_pace_sent_us = esp_timer_get_time();
        xSemaphoreGive(mu);
    }
};

static esp_err_t http_exchange(const char *method, const char *url, const char *device_key, const char *body,
                               const char *if_none_match, int *out_status, char *resp, int resp_cap, size_t *out_n,
                               int io_ms)
{
    if (!url || !url[0]) {
        return ESP_ERR_INVALID_ARG;
    }
    SkillInvokePace invoke_pace(url);
    /* 长 IO（新闻正文等）会占锁十余秒；短等 2s 会导致并发请求误回退摘要 */
    uint32_t lock_ms = 2000;
    if (io_ms > 2000) {
        lock_ms = (uint32_t)io_ms;
    }
    HttpNetGuard net(lock_ms);
    if (!net) {
        return ESP_ERR_INVALID_STATE;
    }
    force_wifi_perf();

    std::string host;
    std::string path;
    int port = 0;
    bool https = false;
    if (!parse_host_port(url, &host, &port, &path, &https)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (https) {
        ESP_LOGW(TAG, "https not supported on LAN path yet: %s", url);
        return ESP_ERR_NOT_SUPPORTED;
    }

    /* 单次短超时 connect（勿先 probe 再连，翻倍延迟） */
    int fd = tcp_connect_ms(host.c_str(), port, NOD_HTTP_CONNECT_MS, io_ms);
    if (fd < 0) {
        ESP_LOGW(TAG, "TCP %s:%d fail (within %dms)", host.c_str(), port, NOD_HTTP_CONNECT_MS);
        return ESP_FAIL;
    }

    const char *payload = body ? body : "";
    size_t payload_len = body ? strlen(body) : 0;
    char hdr[512];
    int hdr_len = snprintf(hdr, sizeof(hdr),
                           "%s %s HTTP/1.1\r\n"
                           "Host: %s:%d\r\n"
                           "Connection: close\r\n"
                           "Content-Type: application/json\r\n"
                           "Content-Length: %u\r\n",
                           method, path.c_str(), host.c_str(), port, (unsigned)payload_len);
    if (hdr_len <= 0 || hdr_len >= (int)sizeof(hdr)) {
        close(fd);
        return ESP_ERR_INVALID_SIZE;
    }
    std::string req(hdr, (size_t)hdr_len);
    if (device_key && device_key[0]) {
        req += "X-Device-Key: ";
        req += device_key;
        req += "\r\n";
    }
    if (if_none_match && if_none_match[0]) {
        req += "If-None-Match: ";
        req += if_none_match;
        req += "\r\n";
    }
    req += "\r\n";
    if (payload_len) {
        req.append(payload, payload_len);
    }

    ESP_LOGI(TAG, "%s %s (short-tcp)", method, url);
    if (!send_all(fd, req.data(), req.size())) {
        ESP_LOGW(TAG, "send fail errno=%d", errno);
        close(fd);
        return ESP_FAIL;
    }
    /* 服务端限流看的是到达时刻，约等于这一下 send 完成 */
    invoke_pace.mark_sent();

    std::vector<char> buf;
    buf.reserve(4096);
    char tmp[1024];
    while (true) {
        int n = recv(fd, tmp, sizeof(tmp), 0);
        if (n > 0) {
            buf.insert(buf.end(), tmp, tmp + n);
            if (buf.size() > 96 * 1024) {
                break;
            }
            continue;
        }
        break;
    }
    close(fd);

    size_t raw_len = buf.size();
    buf.push_back('\0'); /* sscanf / 日志用；body_len 仍按 raw_len */
    const char *raw = buf.data();
    int status = 0;
    if (sscanf(raw, "HTTP/%*s %d", &status) != 1) {
        ESP_LOGW(TAG, "bad status line (recv bytes=%u)", (unsigned)raw_len);
        return ESP_FAIL;
    }
    if (out_status) {
        *out_status = status;
    }
    const char *sep = nullptr;
    for (size_t i = 0; i + 3 < raw_len; i++) {
        if (raw[i] == '\r' && raw[i + 1] == '\n' && raw[i + 2] == '\r' && raw[i + 3] == '\n') {
            sep = raw + i;
            break;
        }
    }
    const char *body_start = sep ? sep + 4 : "";
    size_t resp_hdr_len = sep ? static_cast<size_t>(sep - raw) : 0;
    size_t body_len = sep ? raw_len - (resp_hdr_len + 4) : 0;

    std::string decoded;
    const char *resp_body = body_start;
    size_t resp_n = body_len;
    if (header_has_chunked(raw, resp_hdr_len)) {
        if (!decode_chunked(body_start, body_len, &decoded)) {
            ESP_LOGW(TAG, "chunked decode fail len=%u", (unsigned)body_len);
            return ESP_FAIL;
        }
        resp_body = decoded.c_str();
        resp_n = decoded.size();
    }

    if (resp && resp_cap > 0) {
        if (out_n) {
            if (resp_n > (size_t)resp_cap) {
                ESP_LOGW(TAG, "body %u exceeds cap %d", (unsigned)resp_n, resp_cap);
                return ESP_ERR_INVALID_SIZE;
            }
            memcpy(resp, resp_body, resp_n);
            *out_n = resp_n;
        } else {
            size_t n = resp_n;
            if (n >= (size_t)resp_cap) {
                n = (size_t)resp_cap - 1;
            }
            memcpy(resp, resp_body, n);
            resp[n] = '\0';
        }
    }
    if (status != 200 && status != 201 && status != 304) {
        /* 打出 body 便于定位 401/403（绑定码、密钥、配额等） */
        char trunc[160];
        size_t sn = resp_n < sizeof(trunc) - 1 ? resp_n : sizeof(trunc) - 1;
        memcpy(trunc, resp_body, sn);
        trunc[sn] = '\0';
        ESP_LOGW(TAG, "%s %s → HTTP %d body=%s", method, url, status, trunc);
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "%s %s → HTTP %d", method, url, status);
    return ESP_OK;
}

esp_err_t nodelio_http_post_json(const char *url, const char *device_key, const char *body, int *out_status,
                                 char *resp, int resp_cap)
{
    return nodelio_http_post_json_ms(url, device_key, body, out_status, resp, resp_cap, NOD_HTTP_IO_MS);
}

esp_err_t nodelio_http_post_json_ms(const char *url, const char *device_key, const char *body, int *out_status,
                                    char *resp, int resp_cap, int io_ms)
{
    if (io_ms <= 0) {
        io_ms = NOD_HTTP_IO_MS;
    }
    return http_exchange("POST", url, device_key, body ? body : "{}", nullptr, out_status, resp, resp_cap, nullptr,
                         io_ms);
}

esp_err_t nodelio_http_post_image(const char *url, const char *device_key, const void *data, size_t len,
                                  int *out_status)
{
    if (!url || !url[0] || !data || len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    HttpNetGuard net;
    if (!net) {
        return ESP_ERR_INVALID_STATE;
    }
    force_wifi_perf();

    std::string host;
    std::string path;
    int port = 0;
    bool https = false;
    if (!parse_host_port(url, &host, &port, &path, &https)) {
        return ESP_ERR_INVALID_ARG;
    }

    int fd = tcp_connect_ms(host.c_str(), port, NOD_HTTP_CONNECT_MS, NOD_HTTP_IO_MS);
    if (fd < 0) {
        return ESP_FAIL;
    }

    char hdr[512];
    int hdr_len = snprintf(hdr, sizeof(hdr),
                           "POST %s HTTP/1.1\r\n"
                           "Host: %s:%d\r\n"
                           "Connection: close\r\n"
                           "Content-Type: image/jpeg\r\n"
                           "Content-Length: %u\r\n",
                           path.c_str(), host.c_str(), port, (unsigned)len);
    std::string req(hdr, (size_t)hdr_len);
    if (device_key && device_key[0]) {
        req += "X-Device-Key: ";
        req += device_key;
        req += "\r\n";
    }
    req += "\r\n";

    if (!send_all(fd, req.data(), req.size()) || !send_all(fd, (const char *)data, len)) {
        close(fd);
        return ESP_FAIL;
    }

    char tmp[512];
    int n = recv(fd, tmp, sizeof(tmp) - 1, 0);
    close(fd);
    int status = 0;
    if (n > 0) {
        tmp[n] = '\0';
        sscanf(tmp, "HTTP/%*s %d", &status);
    }
    if (out_status) {
        *out_status = status;
    }
    ESP_LOGI(TAG, "POST image %s (len=%u) → HTTP %d", url, (unsigned)len, status);
    return (status >= 200 && status < 300) ? ESP_OK : ESP_FAIL;
}

esp_err_t nodelio_http_get(const char *url, const char *device_key, const char *if_none_match, int *out_status,
                           char *resp, int resp_cap)
{
    return http_exchange("GET", url, device_key, nullptr, if_none_match, out_status, resp, resp_cap, nullptr,
                         NOD_HTTP_IO_MS);
}

esp_err_t nodelio_http_get_ms(const char *url, const char *device_key, const char *if_none_match, int *out_status,
                              char *resp, int resp_cap, int io_ms)
{
    return http_exchange("GET", url, device_key, nullptr, if_none_match, out_status, resp, resp_cap, nullptr, io_ms);
}

esp_err_t nodelio_http_get_bin(const char *url, const char *device_key, int *out_status, uint8_t *resp, int resp_cap,
                               size_t *out_n)
{
    return nodelio_http_get_bin_ms(url, device_key, out_status, resp, resp_cap, out_n, NOD_HTTP_IO_MS);
}

esp_err_t nodelio_http_get_bin_ms(const char *url, const char *device_key, int *out_status, uint8_t *resp, int resp_cap,
                                  size_t *out_n, int io_ms)
{
    if (!out_n) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_n = 0;
    return http_exchange("GET", url, device_key, nullptr, nullptr, out_status, (char *)resp, resp_cap, out_n, io_ms);
}

esp_err_t nodelio_http_delete(const char *url, const char *device_key, int *out_status, char *resp, int resp_cap)
{
    return http_exchange("DELETE", url, device_key, nullptr, nullptr, out_status, resp, resp_cap, nullptr,
                         NOD_HTTP_IO_MS);
}
