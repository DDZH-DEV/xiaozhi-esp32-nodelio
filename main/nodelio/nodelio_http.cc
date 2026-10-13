#include "nodelio_http.h"

#include <arpa/inet.h>
#include <errno.h>
#include <esp_log.h>
#include <esp_wifi.h>
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
#define NOD_HTTP_CONNECT_MS 3000
#endif
#ifndef NOD_HTTP_IO_MS
#define NOD_HTTP_IO_MS 5000
#endif

static void force_wifi_perf(void)
{
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

/** 短超时 TCP（勿用小智 EspTcp：其 connect 无超时，SYN 可卡数十秒拖垮音频） */
static int tcp_connect_ms(const char *host, int port, int timeout_ms)
{
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
    struct timeval rcv = {.tv_sec = NOD_HTTP_IO_MS / 1000, .tv_usec = (NOD_HTTP_IO_MS % 1000) * 1000};
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
    force_wifi_perf();
    ESP_LOGI(TAG, "TCP probe → %s:%d (%dms)…", host, port, NOD_HTTP_CONNECT_MS);
    int fd = tcp_connect_ms(host, port, NOD_HTTP_CONNECT_MS);
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

static esp_err_t http_exchange(const char *method, const char *url, const char *device_key, const char *body,
                               const char *if_none_match, int *out_status, char *resp, int resp_cap)
{
    if (!url || !url[0]) {
        return ESP_ERR_INVALID_ARG;
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
    int fd = tcp_connect_ms(host.c_str(), port, NOD_HTTP_CONNECT_MS);
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
        ESP_LOGW(TAG, "bad status line");
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
        size_t n = resp_n;
        if (n >= (size_t)resp_cap) {
            n = (size_t)resp_cap - 1;
        }
        memcpy(resp, resp_body, n);
        resp[n] = '\0';
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
    return http_exchange("POST", url, device_key, body ? body : "{}", nullptr, out_status, resp, resp_cap);
}

esp_err_t nodelio_http_get(const char *url, const char *device_key, const char *if_none_match, int *out_status,
                           char *resp, int resp_cap)
{
    return http_exchange("GET", url, device_key, nullptr, if_none_match, out_status, resp, resp_cap);
}
