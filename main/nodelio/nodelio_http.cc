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

#include <esp_heap_caps.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#define TAG "nl_http"

#ifndef NOD_HTTP_CONNECT_MS
#define NOD_HTTP_CONNECT_MS 2000
#endif
#ifndef NOD_HTTP_IO_MS
#define NOD_HTTP_IO_MS 2500
#endif
/** 语音结束后给小智 MQTT TLS 重连的空窗（日志里常见 3–10s 与 LAN HTTP 撞车） */
#ifndef NOD_HTTP_VOICE_GRACE_MS
#define NOD_HTTP_VOICE_GRACE_MS 3500
#endif
/** 短请求也要能排在长 invoke（8s IO）后面，避免 2s 等锁直接 fail */
#ifndef NOD_HTTP_LOCK_MIN_MS
#define NOD_HTTP_LOCK_MIN_MS 8000
#endif
#ifndef NOD_HTTP_LOCK_MAX_MS
#define NOD_HTTP_LOCK_MAX_MS 20000
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

static int64_t s_lan_quiet_until_us;
static bool s_voice_was_active;

static void refresh_voice_grace(void)
{
    if (nodelio_voice_session_active()) {
        s_voice_was_active = true;
        s_lan_quiet_until_us = esp_timer_get_time() + (int64_t)NOD_HTTP_VOICE_GRACE_MS * 1000;
        return;
    }
    if (s_voice_was_active) {
        s_voice_was_active = false;
        s_lan_quiet_until_us = esp_timer_get_time() + (int64_t)NOD_HTTP_VOICE_GRACE_MS * 1000;
        ESP_LOGI(TAG, "voice ended — lan quiet %ums for MQTT", (unsigned)NOD_HTTP_VOICE_GRACE_MS);
    }
}

bool nodelio_http_wait_lan_ready(uint32_t timeout_ms)
{
    if (timeout_ms == 0) {
        refresh_voice_grace();
        return !nodelio_voice_session_active() && esp_timer_get_time() >= s_lan_quiet_until_us;
    }
    const int64_t deadline = esp_timer_get_time() + (int64_t)timeout_ms * 1000;
    for (;;) {
        refresh_voice_grace();
        const int64_t now = esp_timer_get_time();
        if (!nodelio_voice_session_active() && now >= s_lan_quiet_until_us) {
            return true;
        }
        if (now >= deadline) {
            ESP_LOGW(TAG, "lan wait timeout voice=%d quiet_left=%lldms",
                     nodelio_voice_session_active() ? 1 : 0,
                     (long long)((s_lan_quiet_until_us - now) / 1000));
            return false;
        }
        uint32_t slice = 100;
        if (!nodelio_voice_session_active() && s_lan_quiet_until_us > now) {
            const int64_t left_ms = (s_lan_quiet_until_us - now) / 1000;
            if (left_ms > 0 && left_ms < (int64_t)slice) {
                slice = (uint32_t)left_ms;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(slice ? slice : 1));
    }
}

struct HttpNetGuard {
    bool ok = false;
    explicit HttpNetGuard(uint32_t lock_ms = NOD_HTTP_LOCK_MIN_MS)
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

/**
 * 单次 short-tcp。pace_mark 非空时在 send 成功后调用（限流按到达时刻）。
 * out_empty_recv：对端 0 字节 / 无状态行时置 true，供外层在 MQTT 争用窗口重试。
 */
static esp_err_t http_exchange_once(const char *method, const char *url, const char *device_key, const char *body,
                                    const char *if_none_match, int *out_status, char *resp, int resp_cap, size_t *out_n,
                                    int io_ms, const char *content_type, const char *x_pass, SkillInvokePace *pace,
                                    bool *out_empty_recv, int connect_ms = -1)
{
    if (out_empty_recv) {
        *out_empty_recv = false;
    }
    if (!url || !url[0]) {
        return ESP_ERR_INVALID_ARG;
    }
    if (connect_ms <= 0) {
        connect_ms = NOD_HTTP_CONNECT_MS;
    }
    /* 长 IO（新闻正文等）会占锁十余秒；短等 2s 会导致并发请求误回退摘要 */
    uint32_t lock_ms = (uint32_t)(io_ms > 0 ? io_ms : NOD_HTTP_IO_MS);
    if (lock_ms < NOD_HTTP_LOCK_MIN_MS) {
        lock_ms = NOD_HTTP_LOCK_MIN_MS;
    }
    if (lock_ms > NOD_HTTP_LOCK_MAX_MS) {
        lock_ms = NOD_HTTP_LOCK_MAX_MS;
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
    int fd = tcp_connect_ms(host.c_str(), port, connect_ms, io_ms);
    if (fd < 0) {
        ESP_LOGW(TAG, "TCP %s:%d fail (within %dms)", host.c_str(), port, connect_ms);
        /* 连不上 ≠ 空收：勿标 empty_recv，外层不应再刷 3×connect */
        return ESP_ERR_TIMEOUT;
    }

    const char *payload = body ? body : "";
    size_t payload_len = body ? strlen(body) : 0;
    const char *ctype = (content_type && content_type[0]) ? content_type : "application/json";
    char hdr[512];
    int hdr_len = snprintf(hdr, sizeof(hdr),
                           "%s %s HTTP/1.1\r\n"
                           "Host: %s:%d\r\n"
                           "Connection: close\r\n"
                           "Content-Type: %s\r\n"
                           "Content-Length: %u\r\n",
                           method, path.c_str(), host.c_str(), port, ctype, (unsigned)payload_len);
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
    if (x_pass && x_pass[0]) {
        req += "X-Pass: ";
        req += x_pass;
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
        if (out_empty_recv) {
            *out_empty_recv = true;
        }
        return ESP_FAIL;
    }
    /* 服务端限流看的是到达时刻，约等于这一下 send 完成 */
    if (pace) {
        pace->mark_sent();
    }

    /* 收包放 PSRAM。默认 new/vector 扩容失败会走异常桩 abort（片内 DRAM 碎片时常见）。 */
    constexpr size_t kMaxRaw = 96 * 1024;
    size_t cap = 4096;
    char *raw_buf =
        static_cast<char *>(heap_caps_malloc(cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!raw_buf) {
        raw_buf = static_cast<char *>(malloc(cap));
    }
    if (!raw_buf) {
        close(fd);
        ESP_LOGW(TAG, "recv buf alloc fail");
        return ESP_ERR_NO_MEM;
    }
    size_t raw_len = 0;
    char tmp[1024];
    while (true) {
        int n = recv(fd, tmp, sizeof(tmp), 0);
        if (n <= 0) {
            break;
        }
        if (raw_len + (size_t)n + 1 > cap) {
            size_t ncap = cap * 2;
            if (ncap < raw_len + (size_t)n + 1) {
                ncap = raw_len + (size_t)n + 1;
            }
            if (ncap > kMaxRaw + 1) {
                ncap = kMaxRaw + 1;
            }
            if (raw_len + (size_t)n + 1 > ncap) {
                ESP_LOGW(TAG, "recv truncated at %u", (unsigned)raw_len);
                break;
            }
            char *nb = static_cast<char *>(
                heap_caps_realloc(raw_buf, ncap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
            if (!nb) {
                nb = static_cast<char *>(realloc(raw_buf, ncap));
            }
            if (!nb) {
                free(raw_buf);
                close(fd);
                ESP_LOGW(TAG, "recv grow fail need=%u", (unsigned)ncap);
                return ESP_ERR_NO_MEM;
            }
            raw_buf = nb;
            cap = ncap;
        }
        memcpy(raw_buf + raw_len, tmp, (size_t)n);
        raw_len += (size_t)n;
        if (raw_len >= kMaxRaw) {
            break;
        }
    }
    close(fd);
    raw_buf[raw_len] = '\0';

    const char *raw = raw_buf;
    int status = 0;
    if (sscanf(raw, "HTTP/%*s %d", &status) != 1) {
        ESP_LOGW(TAG, "bad status line (recv bytes=%u)", (unsigned)raw_len);
        free(raw_buf);
        if (out_empty_recv) {
            *out_empty_recv = true;
        }
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
            free(raw_buf);
            return ESP_FAIL;
        }
        resp_body = decoded.c_str();
        resp_n = decoded.size();
    }

    esp_err_t ret = ESP_OK;
    if (resp && resp_cap > 0) {
        if (out_n) {
            if (resp_n > (size_t)resp_cap) {
                ESP_LOGW(TAG, "body %u exceeds cap %d", (unsigned)resp_n, resp_cap);
                ret = ESP_ERR_INVALID_SIZE;
            } else {
                memcpy(resp, resp_body, resp_n);
                *out_n = resp_n;
            }
        } else {
            size_t n = resp_n;
            if (n >= (size_t)resp_cap) {
                n = (size_t)resp_cap - 1;
            }
            memcpy(resp, resp_body, n);
            resp[n] = '\0';
        }
    }
    if (ret == ESP_OK && status != 200 && status != 201 && status != 304) {
        /* 打出 body 便于定位 401/403（绑定码、密钥、配额等） */
        char trunc[160];
        size_t sn = resp_n < sizeof(trunc) - 1 ? resp_n : sizeof(trunc) - 1;
        memcpy(trunc, resp_body, sn);
        trunc[sn] = '\0';
        ESP_LOGW(TAG, "%s %s → HTTP %d body=%s", method, url, status, trunc);
        /* 技能 invoke 的 config_missing 等会回 403 + Envelope；body 已写入 resp，交给上层上屏/toast */
        const bool skill_envelope_err =
            status >= 400 && status < 500 && resp_n > 0 && resp_body &&
            strstr(resp_body, "\"schema_version\"") != nullptr;
        if (!skill_envelope_err) {
            ret = ESP_FAIL;
        }
    } else if (ret == ESP_OK) {
        ESP_LOGI(TAG, "%s %s → HTTP %d", method, url, status);
    }
    free(raw_buf);
    return ret;
}

static esp_err_t http_exchange(const char *method, const char *url, const char *device_key, const char *body,
                               const char *if_none_match, int *out_status, char *resp, int resp_cap, size_t *out_n,
                               int io_ms, const char *content_type, const char *x_pass)
{
    if (!url || !url[0]) {
        return ESP_ERR_INVALID_ARG;
    }
    if (io_ms <= 0) {
        io_ms = NOD_HTTP_IO_MS;
    }

    /* 用户触发的技能/正文请求不能等语音结束：新闻打开后还要在同一会话问「要读吗」。
     * 心跳等后台请求仍让路给 MQTT。 */
    const bool ui_content =
        strstr(url, "/panel/news/") != nullptr || strstr(url, "/panel/snapshot") != nullptr ||
        (strstr(url, "/panel/skills/") != nullptr && strstr(url, "/invoke") != nullptr);
    if (!ui_content) {
        uint32_t wait_ms = 4000;
        if ((uint32_t)io_ms > wait_ms) {
            wait_ms = (uint32_t)io_ms;
        }
        if (wait_ms > 10000) {
            wait_ms = 10000;
        }
        (void)nodelio_http_wait_lan_ready(wait_ms);
    }

    SkillInvokePace invoke_pace(url);
    esp_err_t last = ESP_FAIL;
    /* 派活/审批可能已执行但响应丢失，没有幂等键时不能自动重发。 */
    const bool dsh_mutation = strcmp(method, "POST") == 0 &&
                              strstr(url, "/panel/skills/") != nullptr &&
                              (strstr(url, "dsh.task") || strstr(url, "dsh.answer"));
    const int kMaxAttempts = dsh_mutation ? 1 : 3;
    for (int attempt = 0; attempt < kMaxAttempts; attempt++) {
        if (attempt > 0) {
            ESP_LOGW(TAG, "retry %s %s (%d/%d)", method, url, attempt + 1, kMaxAttempts);
            vTaskDelay(pdMS_TO_TICKS(400u * (uint32_t)attempt));
            if (!ui_content) {
                (void)nodelio_http_wait_lan_ready(2000);
            }
            if (out_status) {
                *out_status = 0;
            }
            if (resp && resp_cap > 0) {
                resp[0] = '\0';
            }
            if (out_n) {
                *out_n = 0;
            }
        }
        bool empty = false;
        /* 仅首发走 pace；重试不再二次 Sleep（服务端已按首次到达限流） */
        SkillInvokePace *pace = (attempt == 0) ? &invoke_pace : nullptr;
        last = http_exchange_once(method, url, device_key, body, if_none_match, out_status, resp, resp_cap, out_n,
                                  io_ms, content_type, x_pass, pace, &empty);
        /* once 若已 send 会自行 mark；此处兜底释放，避免重试间隔堵住其它 invoke */
        invoke_pace.mark_sent();
        if (last == ESP_OK) {
            return last;
        }
        /* 业务 4xx（Envelope 已写入）不重试；锁忙 / 空收才重试 */
        if (last == ESP_ERR_INVALID_ARG || last == ESP_ERR_NOT_SUPPORTED || last == ESP_ERR_NO_MEM ||
            last == ESP_ERR_INVALID_SIZE) {
            break;
        }
        /* 主机不可达：开机 register 再刷 2×2s 只会拖死 Xiaozhi MQTT；deferred/hb 稍后补 */
        if (last == ESP_ERR_TIMEOUT) {
            break;
        }
        if (last == ESP_ERR_INVALID_STATE || empty) {
            continue;
        }
        /* 已有 HTTP 状态码的失败（401/500 等）不再刷 */
        if (out_status && *out_status > 0) {
            break;
        }
    }
    return last;
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
                         io_ms, nullptr, nullptr);
}

esp_err_t nodelio_http_post_image(const char *url, const char *device_key, const void *data, size_t len,
                                  int *out_status)
{
    if (out_status) {
        *out_status = 0;
    }
    if (!url || !url[0] || !data || len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    /* 截图体大，等网锁更久；心跳/技能并发时 2s 易误判 */
    HttpNetGuard net(8000);
    if (!net) {
        ESP_LOGW(TAG, "POST image skip: net not ready / lock busy");
        return ESP_ERR_INVALID_STATE;
    }
    force_wifi_perf();

    const size_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
    /* 碎片下 largest 常仅 6～8KB；短 TCP 心跳仍成功，勿 12KB 误杀 */
    if (largest < 2 * 1024) {
        ESP_LOGW(TAG, "POST image abort: largest_dram=%u (TCP buf)", (unsigned)largest);
        return ESP_ERR_NO_MEM;
    }

    std::string host;
    std::string path;
    int port = 0;
    bool https = false;
    if (!parse_host_port(url, &host, &port, &path, &https)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (https) {
        ESP_LOGW(TAG, "POST image: https not supported on this path");
        return ESP_ERR_NOT_SUPPORTED;
    }

    /* 大图：连接/发送超时加长（默认 2.5s 易在弱网半截失败 → http=0） */
    const int connect_ms = 5000;
    const int io_ms = 20000;
    int fd = tcp_connect_ms(host.c_str(), port, connect_ms, io_ms);
    if (fd < 0) {
        ESP_LOGW(TAG, "POST image connect fail %s:%d len=%u dram=%u", host.c_str(), port, (unsigned)len,
                 (unsigned)largest);
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

    if (!send_all(fd, req.data(), req.size())) {
        ESP_LOGW(TAG, "POST image hdr send fail errno=%d", errno);
        close(fd);
        return ESP_FAIL;
    }
    if (!send_all(fd, (const char *)data, len)) {
        ESP_LOGW(TAG, "POST image body send fail errno=%d sent_target=%u", errno, (unsigned)len);
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
    } else {
        ESP_LOGW(TAG, "POST image no response errno=%d n=%d", errno, n);
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
                         NOD_HTTP_IO_MS, nullptr, nullptr);
}

esp_err_t nodelio_http_get_ms(const char *url, const char *device_key, const char *if_none_match, int *out_status,
                              char *resp, int resp_cap, int io_ms)
{
    return http_exchange("GET", url, device_key, nullptr, if_none_match, out_status, resp, resp_cap, nullptr, io_ms,
                         nullptr, nullptr);
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
    return http_exchange("GET", url, device_key, nullptr, nullptr, out_status, (char *)resp, resp_cap, out_n, io_ms,
                         nullptr, nullptr);
}

esp_err_t nodelio_http_delete(const char *url, const char *device_key, int *out_status, char *resp, int resp_cap)
{
    return http_exchange("DELETE", url, device_key, nullptr, nullptr, out_status, resp, resp_cap, nullptr,
                         NOD_HTTP_IO_MS, nullptr, nullptr);
}

esp_err_t nodelio_http_lan_get_ms(const char *url, const char *x_pass, int *out_status, char *resp, int resp_cap,
                                  int io_ms)
{
    if (io_ms <= 0) {
        io_ms = NOD_HTTP_IO_MS;
    }
    return http_exchange("GET", url, nullptr, nullptr, nullptr, out_status, resp, resp_cap, nullptr, io_ms, nullptr,
                         x_pass);
}

esp_err_t nodelio_http_lan_probe_get(const char *url, const char *x_pass, int *out_status, char *resp, int resp_cap)
{
    /* DIOL 普查：单次、短 connect；死 IP 勿 2s×3 拖死开机 */
    constexpr int kConnectMs = 800;
    constexpr int kIoMs = 1200;
    if (!url || !url[0]) {
        return ESP_ERR_INVALID_ARG;
    }
    (void)nodelio_http_wait_lan_ready(1500);
    if (out_status) {
        *out_status = 0;
    }
    if (resp && resp_cap > 0) {
        resp[0] = '\0';
    }
    bool empty = false;
    return http_exchange_once("GET", url, nullptr, nullptr, nullptr, out_status, resp, resp_cap, nullptr, kIoMs,
                              nullptr, x_pass, nullptr, &empty, kConnectMs);
}

esp_err_t nodelio_http_lan_post_form_ms(const char *url, const char *x_pass, const char *form_body, int *out_status,
                                        char *resp, int resp_cap, int io_ms)
{
    if (io_ms <= 0) {
        io_ms = NOD_HTTP_IO_MS;
    }
    return http_exchange("POST", url, nullptr, form_body ? form_body : "", nullptr, out_status, resp, resp_cap, nullptr,
                         io_ms, "application/x-www-form-urlencoded", x_pass);
}
