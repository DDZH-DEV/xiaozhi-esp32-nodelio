#include "xz_activation.h"

#include "settings.h"

#include <cstdio>
#include <cstring>

void nodelio_xz_activation_store(const char *code, const char *msg)
{
    if (!code || !code[0]) {
        return;
    }
    Settings wifi("wifi", true);
    wifi.SetString("xz_act_code", code);
    if (msg && msg[0]) {
        char trunc[96];
        std::snprintf(trunc, sizeof(trunc), "%s", msg);
        wifi.SetString("xz_act_msg", trunc);
    } else {
        wifi.EraseKey("xz_act_msg");
    }
}

void nodelio_xz_activation_clear(void)
{
    Settings wifi("wifi", true);
    wifi.EraseKey("xz_act_code");
    wifi.EraseKey("xz_act_msg");
}

bool nodelio_xz_activation_get(char *out_code, size_t code_cap, char *out_msg, size_t msg_cap)
{
    Settings wifi("wifi", false);
    const std::string code = wifi.GetString("xz_act_code");
    if (code.empty()) {
        if (out_code && code_cap) {
            out_code[0] = '\0';
        }
        if (out_msg && msg_cap) {
            out_msg[0] = '\0';
        }
        return false;
    }
    if (out_code && code_cap) {
        std::snprintf(out_code, code_cap, "%s", code.c_str());
    }
    if (out_msg && msg_cap) {
        const std::string msg = wifi.GetString("xz_act_msg");
        std::snprintf(out_msg, msg_cap, "%s", msg.c_str());
    }
    return true;
}
