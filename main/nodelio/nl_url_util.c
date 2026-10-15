#include "nl_url_util.h"

#include <stdio.h>
#include <string.h>

void nl_url_strip_base(const char *base, char *out, size_t out_len)
{
    if (!out || out_len == 0) {
        return;
    }
    out[0] = '\0';
    if (!base || !base[0]) {
        return;
    }
    size_t n = strlen(base);
    while (n > 0 && base[n - 1] == '/') {
        n--;
    }
    if (n >= 22 && strncmp(base + n - 22, "/api/devices/register", 22) == 0) {
        n -= 22;
    }
    snprintf(out, out_len, "%.*s", (int)n, base);
}

void nl_url_parse_server(const char *url, char *host, size_t host_len, char *port, size_t port_len,
                         bool *https)
{
    if (host && host_len) {
        host[0] = '\0';
    }
    if (port && port_len) {
        snprintf(port, port_len, "8899");
    }
    if (https) {
        *https = false;
    }
    if (!url || !url[0] || !host || host_len == 0) {
        return;
    }
    const char *p = url;
    if (strncmp(p, "https://", 8) == 0) {
        if (https) {
            *https = true;
        }
        p += 8;
    } else if (strncmp(p, "http://", 7) == 0) {
        p += 7;
    }
    const char *slash = strchr(p, '/');
    const char *colon = NULL;
    for (const char *q = p; *q && q != slash; q++) {
        if (*q == ':') {
            colon = q;
        }
    }
    size_t hn = colon ? (size_t)(colon - p) : (slash ? (size_t)(slash - p) : strlen(p));
    if (hn >= host_len) {
        hn = host_len - 1;
    }
    memcpy(host, p, hn);
    host[hn] = '\0';
    if (colon && colon[1] && port && port_len) {
        size_t pn = 0;
        for (const char *q = colon + 1; *q && *q != '/' && pn + 1 < port_len; q++) {
            port[pn++] = *q;
        }
        port[pn] = '\0';
    }
}
