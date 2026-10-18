#pragma once

#include <cJSON.h>
#include <esp_http_server.h>

#ifdef __cplusplus
extern "C" {
#endif

/** SoftAP 门户首页（替代小智默认页；含 WiFi + 服务器/绑定码）. */
const char *nodelio_softap_index_html(void);

/** /submit 成功连上 WiFi 后：落盘 server_url / owner，并安排重启. */
void nodelio_softap_on_wifi_submit(cJSON *json);

/** 注册 /nodelio/config、/nodelio/submit（仅改服务器也可）. */
void nodelio_softap_register_handlers(httpd_handle_t server);

#ifdef __cplusplus
}
#endif
