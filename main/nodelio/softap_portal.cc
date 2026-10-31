#include "softap_portal.h"

#include "nl_url_util.h"
#include "nvs_config.h"

#include <esp_log.h>
#include <esp_system.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#define TAG "nl_softap"

static void schedule_reboot(void)
{
    xTaskCreate(
        [](void *) {
            vTaskDelay(pdMS_TO_TICKS(800));
            ESP_LOGW(TAG, "SoftAP provision done — reboot");
            esp_restart();
        },
        "nl_ap_reboot", 2048, nullptr, 5, nullptr);
}

static bool apply_server_fields(cJSON *json, nodelio_nvs_config_t *cfg, bool *server_changed)
{
    if (!json || !cfg || !server_changed) {
        return false;
    }
    *server_changed = false;

    cJSON *owner_item = cJSON_GetObjectItemCaseSensitive(json, "owner");
    cJSON *host_item = cJSON_GetObjectItemCaseSensitive(json, "host");
    cJSON *port_item = cJSON_GetObjectItemCaseSensitive(json, "port");
    cJSON *https_item = cJSON_GetObjectItemCaseSensitive(json, "https");
    cJSON *url_item = cJSON_GetObjectItemCaseSensitive(json, "server_url");

    char new_url[sizeof(cfg->server_url)] = {0};
    if (cJSON_IsString(url_item) && url_item->valuestring && url_item->valuestring[0]) {
        std::snprintf(new_url, sizeof(new_url), "%s", url_item->valuestring);
    } else if (cJSON_IsString(host_item) && host_item->valuestring && host_item->valuestring[0]) {
        const char *port = "8899";
        if (cJSON_IsString(port_item) && port_item->valuestring && port_item->valuestring[0]) {
            port = port_item->valuestring;
        }
        bool https = cJSON_IsTrue(https_item);
        std::snprintf(new_url, sizeof(new_url), "%s://%s:%s", https ? "https" : "http",
                      host_item->valuestring, port);
    }

    if (new_url[0]) {
        if (std::strcmp(cfg->server_url, new_url) != 0) {
            *server_changed = true;
        }
        std::snprintf(cfg->server_url, sizeof(cfg->server_url), "%s", new_url);
    }

    if (cJSON_IsString(owner_item) && owner_item->valuestring && owner_item->valuestring[0]) {
        if (std::strcmp(cfg->owner, owner_item->valuestring) != 0) {
            *server_changed = true;
        }
        std::snprintf(cfg->owner, sizeof(cfg->owner), "%s", owner_item->valuestring);
    }

    return cfg->server_url[0] != '\0' && cfg->owner[0] != '\0';
}

extern "C" const char *nodelio_softap_index_html(void)
{
    /* 紧凑中文门户：WiFi + 服务器；复用小智 /scan /submit /exit */
    static const char kHtml[] =
        "<!DOCTYPE html><html><head><meta charset=utf-8>"
        "<meta name=viewport content=\"width=device-width,initial-scale=1,user-scalable=no\">"
        "<title>NodeLio 配网</title>"
        "<style>"
        "body{font-family:system-ui,sans-serif;margin:0;padding:16px;background:#0c0f14;color:#e2e8f0}"
        ".card{max-width:440px;margin:0 auto;background:#161b24;border:1px solid #222a38;border-radius:14px;padding:18px}"
        "h1{font-size:18px;margin:0 0 4px;color:#34c759}p.sub{margin:0 0 16px;color:#94a3b8;font-size:13px}"
        "label{display:block;margin:12px 0 6px;font-size:13px;color:#94a3b8}"
        "input,select{width:100%;box-sizing:border-box;padding:11px 12px;border-radius:10px;border:1px solid #334155;"
        "background:#0c0f14;color:#e2e8f0;font-size:15px}"
        "button{width:100%;margin-top:16px;padding:13px;border:0;border-radius:10px;background:#34c759;color:#fff;"
        "font-size:15px;font-weight:600}"
        "button:disabled{opacity:.5}.err{color:#f87171;font-size:13px;min-height:1.2em}"
        "#aps a{display:block;margin-top:6px;padding:8px 12px;background:#0c0f14;border:1px solid #334155;"
        "border-radius:10px;color:#e2e8f0;text-decoration:none;font-size:13px}"
        ".row{display:flex;gap:10px;align-items:center}.row input[type=checkbox]{width:20px;height:20px}"
        "</style></head><body><div class=card>"
        "<h1>NodeLio 配网</h1>"
        "<p class=sub>连接本热点后填写 WiFi 与服务器，保存后设备将重启。</p>"
        "<div class=err id=err></div>"
        "<label>WiFi 名称 (SSID)</label><input id=ssid autocomplete=off>"
        "<label>WiFi 密码</label><input id=pass type=password>"
        "<div id=aps><p style=color:#94a3b8;font-size:13px>扫描中…</p></div>"
        "<label>服务器主机</label><input id=host placeholder=192.168.1.10>"
        "<label>端口</label><input id=port value=8899>"
        "<div class=row style=margin-top:12px><input type=checkbox id=https><label for=https style=margin:0>HTTPS</label></div>"
        "<label>绑定码 / 账号</label><input id=owner placeholder=手机号或绑定码>"
        "<button id=btn type=button>保存并连接</button>"
        "<button id=btnSrv type=button style=background:#0ea5e9;margin-top:10px>仅保存服务器</button>"
        "</div><script>"
        "const err=document.getElementById('err');"
        "const ssid=document.getElementById('ssid');"
        "const host=document.getElementById('host');"
        "const port=document.getElementById('port');"
        "const https=document.getElementById('https');"
        "const owner=document.getElementById('owner');"
        "const pass=document.getElementById('pass');"
        "const btn=document.getElementById('btn');"
        "const btnSrv=document.getElementById('btnSrv');"
        "async function loadCfg(){"
        "try{const r=await fetch('/nodelio/config');const j=await r.json();"
        "if(j.host)host.value=j.host;if(j.port)port.value=j.port;"
        "https.checked=!!j.https;if(j.owner)owner.value=j.owner;"
        "if(j.ssid)ssid.value=j.ssid;}catch(e){}}"
        "async function loadAps(){"
        "try{const r=await fetch('/scan');const j=await r.json();"
        "const box=document.getElementById('aps');"
        "box.innerHTML='<p style=color:#94a3b8;font-size:13px>点击下方 WiFi：</p>';"
        "(j.aps||[]).forEach(ap=>{"
        "const a=document.createElement('a');a.href='#';"
        "a.textContent=ap.ssid+' ('+ap.rssi+' dBm)'+(ap.authmode? ' 🔒':'');"
        "a.onclick=e=>{e.preventDefault();ssid.value=ap.ssid;};"
        "box.appendChild(a);});"
        "setTimeout(loadAps,5000);}catch(e){setTimeout(loadAps,5000);}}"
        "async function saveAll(wifi){"
        "err.textContent='';btn.disabled=true;btnSrv.disabled=true;"
        "const body={host:host.value.trim(),port:port.value.trim()||'8899',"
        "https:https.checked,owner:owner.value.trim()};"
        "if(!body.host||!body.owner){err.textContent='请填写主机与绑定码';btn.disabled=false;btnSrv.disabled=false;return;}"
        "try{"
        "if(wifi){"
        "if(!ssid.value.trim()){err.textContent='请选择或填写 WiFi';btn.disabled=false;btnSrv.disabled=false;return;}"
        "body.ssid=ssid.value.trim();body.password=pass.value;"
        "const r=await fetch('/submit',{method:'POST',headers:{'Content-Type':'application/json'},"
        "body:JSON.stringify(body)});const j=await r.json();"
        "if(!j.success)throw new Error(j.error||'连接失败');"
        "location.href='/done.html?message='+encodeURIComponent('已保存，设备重启中…');"
        "}else{"
        "const r=await fetch('/nodelio/submit',{method:'POST',headers:{'Content-Type':'application/json'},"
        "body:JSON.stringify(body)});const j=await r.json();"
        "if(!j.success)throw new Error(j.error||'保存失败');"
        "location.href='/done.html?message='+encodeURIComponent('服务器已保存，设备重启中…');"
        "}}catch(e){err.textContent=e.message||String(e);btn.disabled=false;btnSrv.disabled=false;}}"
        "btn.onclick=()=>saveAll(true);btnSrv.onclick=()=>saveAll(false);"
        "loadCfg();loadAps();"
        "</script></body></html>";
    return kHtml;
}

extern "C" void nodelio_softap_on_wifi_submit(cJSON *json)
{
    if (!json) {
        return;
    }
    nodelio_nvs_config_t cfg{};
    (void)nodelio_nvs_load(&cfg);

    cJSON *ssid_item = cJSON_GetObjectItemCaseSensitive(json, "ssid");
    cJSON *pass_item = cJSON_GetObjectItemCaseSensitive(json, "password");
    if (cJSON_IsString(ssid_item) && ssid_item->valuestring) {
        std::snprintf(cfg.wifi_ssid, sizeof(cfg.wifi_ssid), "%s", ssid_item->valuestring);
    }
    if (cJSON_IsString(pass_item) && pass_item->valuestring) {
        std::snprintf(cfg.wifi_pass, sizeof(cfg.wifi_pass), "%s", pass_item->valuestring);
    }

    bool server_changed = false;
    if (!apply_server_fields(json, &cfg, &server_changed)) {
        ESP_LOGW(TAG, "WiFi saved but server/owner incomplete — still reboot");
    }
    if (server_changed) {
        nodelio_nvs_clear_mqtt(&cfg);
    }
    esp_err_t err = nodelio_nvs_save_connection(&cfg);
    ESP_LOGI(TAG, "SoftAP WiFi+server save err=%s url=%s owner=%s", esp_err_to_name(err),
             cfg.server_url, cfg.owner);
    if (err == ESP_OK) {
        schedule_reboot();
    } else {
        ESP_LOGE(TAG, "Provision save failed; reboot cancelled");
    }
}

static esp_err_t handle_config_get(httpd_req_t *req)
{
    nodelio_nvs_config_t cfg{};
    (void)nodelio_nvs_load(&cfg);
    char host[96] = {0};
    char port[8] = {0};
    bool https = false;
    nl_url_parse_server(cfg.server_url, host, sizeof(host), port, sizeof(port), &https);

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "host", host);
    cJSON_AddStringToObject(root, "port", port[0] ? port : "8899");
    cJSON_AddBoolToObject(root, "https", https);
    cJSON_AddStringToObject(root, "owner", cfg.owner);
    cJSON_AddStringToObject(root, "ssid", cfg.wifi_ssid);
    cJSON_AddStringToObject(root, "server_url", cfg.server_url);
    char *out = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!out) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "oom");
        return ESP_FAIL;
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Connection", "close");
    httpd_resp_send(req, out, strlen(out));
    free(out);
    return ESP_OK;
}

static esp_err_t handle_server_submit(httpd_req_t *req)
{
    size_t len = req->content_len;
    if (len == 0 || len > 1024) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad body");
        return ESP_FAIL;
    }
    char *buf = static_cast<char *>(malloc(len + 1));
    if (!buf) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "oom");
        return ESP_FAIL;
    }
    int n = httpd_req_recv(req, buf, len);
    if (n <= 0) {
        free(buf);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "recv");
        return ESP_FAIL;
    }
    buf[n] = '\0';
    cJSON *json = cJSON_Parse(buf);
    free(buf);
    if (!json) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "json");
        return ESP_FAIL;
    }

    nodelio_nvs_config_t cfg{};
    (void)nodelio_nvs_load(&cfg);
    bool server_changed = false;
    if (!apply_server_fields(json, &cfg, &server_changed)) {
        cJSON_Delete(json);
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, "{\"success\":false,\"error\":\"请填写主机与绑定码\"}", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }
    cJSON_Delete(json);
    if (server_changed) {
        nodelio_nvs_clear_mqtt(&cfg);
    }
    esp_err_t err = nodelio_nvs_save_connection(&cfg);
    if (err != ESP_OK) {
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, "{\"success\":false,\"error\":\"NVS 失败\"}", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Connection", "close");
    httpd_resp_send(req, "{\"success\":true}", HTTPD_RESP_USE_STRLEN);
    schedule_reboot();
    return ESP_OK;
}

extern "C" void nodelio_softap_register_handlers(httpd_handle_t server)
{
    if (!server) {
        return;
    }
    httpd_uri_t cfg_get = {
        .uri = "/nodelio/config",
        .method = HTTP_GET,
        .handler = handle_config_get,
        .user_ctx = nullptr,
    };
    httpd_uri_t srv_post = {
        .uri = "/nodelio/submit",
        .method = HTTP_POST,
        .handler = handle_server_submit,
        .user_ctx = nullptr,
    };
    ESP_ERROR_CHECK_WITHOUT_ABORT(httpd_register_uri_handler(server, &cfg_get));
    ESP_ERROR_CHECK_WITHOUT_ABORT(httpd_register_uri_handler(server, &srv_post));
    ESP_LOGI(TAG, "NodeLio SoftAP handlers registered");
}
