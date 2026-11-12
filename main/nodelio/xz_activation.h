#pragma once

#include <cstddef>
#include <string>

#if __cplusplus
extern "C" {
#endif

/** 写入小智激活码（wifi/xz_act_code + xz_act_msg）。msg 可空。 */
void nodelio_xz_activation_store(const char *code, const char *msg);

/** 清除已绑定后的激活码缓存。 */
void nodelio_xz_activation_clear(void);

/** 读出激活码；无码返回 false。out_msg 可空。 */
bool nodelio_xz_activation_get(char *out_code, size_t code_cap, char *out_msg, size_t msg_cap);

#if __cplusplus
}
#endif

#if __cplusplus
#include "ota.h"

/** 有 mqtt/websocket 则清码；否则有激活码则落盘。 */
inline void nodelio_xz_activation_apply_from_ota(Ota &ota)
{
    if (ota.HasMqttConfig() || ota.HasWebsocketConfig()) {
        nodelio_xz_activation_clear();
        return;
    }
    if (ota.HasActivationCode()) {
        nodelio_xz_activation_store(ota.GetActivationCode().c_str(),
                                    ota.GetActivationMessage().c_str());
    }
}

inline bool nodelio_xz_activation_has_code()
{
    char code[32];
    return nodelio_xz_activation_get(code, sizeof(code), nullptr, 0);
}

inline std::string nodelio_xz_activation_code()
{
    char code[32] = {};
    nodelio_xz_activation_get(code, sizeof(code), nullptr, 0);
    return code;
}

inline std::string nodelio_xz_activation_message()
{
    char msg[96] = {};
    nodelio_xz_activation_get(nullptr, 0, msg, sizeof(msg));
    return msg;
}
#endif
