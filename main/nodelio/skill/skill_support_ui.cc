#include "skill_support_ui.h"
#include "skill_ui_priv.h"

#include <cstdio>
#include <cstring>

static void chip_param_text(const char *params_json, char *out, size_t out_len)
{
    if (!out || out_len == 0) {
        return;
    }
    out[0] = '\0';
    if (!params_json || !params_json[0]) {
        return;
    }
    cJSON *root = cJSON_Parse(params_json);
    const cJSON *jt = root ? cJSON_GetObjectItemCaseSensitive(root, "text") : nullptr;
    if (cJSON_IsString(jt) && jt->valuestring && jt->valuestring[0]) {
        snprintf(out, out_len, "%s", jt->valuestring);
    }
    cJSON_Delete(root);
}

static void on_support_chip(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) {
        return;
    }
    const int idx = (int)(intptr_t)lv_event_get_user_data(e);
    if (!s_have_env || !s_env || idx < 0 || idx >= s_env->chip_n) {
        return;
    }
    run_invoke_action(&s_env->chips[idx]);
}

static lv_obj_t *support_bubble(lv_obj_t *parent, const nl_env_chat_msg_t *m, bool left)
{
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_remove_style_all(row);
    lv_obj_set_width(row, lv_pct(100));
    lv_obj_set_height(row, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, left ? LV_FLEX_ALIGN_START : LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_ver(row, 4, 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_CLICKABLE);

    if (!left) {
        lv_obj_t *pad = lv_obj_create(row);
        lv_obj_remove_style_all(pad);
        lv_obj_set_flex_grow(pad, 1);
        lv_obj_set_height(pad, 1);
        lv_obj_clear_flag(pad, LV_OBJ_FLAG_CLICKABLE);
    }

    lv_obj_t *bubble = lv_obj_create(row);
    lv_obj_remove_style_all(bubble);
    lv_obj_set_width(bubble, lv_pct(78));
    lv_obj_set_height(bubble, LV_SIZE_CONTENT);
    lv_obj_set_style_max_width(bubble, lv_pct(78), 0);
    lv_obj_set_style_bg_color(bubble, left ? UI_FIELD : UI_ACCENT_SOFT, 0);
    lv_obj_set_style_bg_opa(bubble, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(bubble, 14, 0);
    lv_obj_set_style_pad_hor(bubble, 12, 0);
    lv_obj_set_style_pad_ver(bubble, 8, 0);
    lv_obj_set_style_pad_row(bubble, 2, 0);
    lv_obj_set_flex_flow(bubble, LV_FLEX_FLOW_COLUMN);
    lv_obj_clear_flag(bubble, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(bubble, LV_OBJ_FLAG_CLICKABLE);

    if (m->name[0]) {
        lv_obj_t *name = lv_label_create(bubble);
        nl_ov_set_font(name);
        lv_label_set_text(name, m->name);
        lv_obj_set_style_text_color(name, UI_TEXT_MUTED, 0);
        lv_obj_set_width(name, lv_pct(100));
        lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);
    }

    lv_obj_t *body = lv_label_create(bubble);
    nl_ov_set_font(body);
    lv_label_set_text(body, m->text[0] ? m->text : " ");
    lv_obj_set_style_text_color(body, UI_TEXT, 0);
    lv_obj_set_width(body, lv_pct(100));
    lv_label_set_long_mode(body, LV_LABEL_LONG_WRAP);

    if (left) {
        lv_obj_t *pad = lv_obj_create(row);
        lv_obj_remove_style_all(pad);
        lv_obj_set_flex_grow(pad, 1);
        lv_obj_set_height(pad, 1);
        lv_obj_clear_flag(pad, LV_OBJ_FLAG_CLICKABLE);
    }
    return row;
}

void fill_support_thread(void)
{
    if (!s_card_list || !s_env) {
        return;
    }
    lv_obj_clean(s_card_list);
    lv_obj_set_flex_flow(s_card_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_card_list, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(s_card_list, 2, 0);
    lv_obj_set_style_pad_hor(s_card_list, 4, 0);
    lv_obj_set_scroll_dir(s_card_list, LV_DIR_VER);
    lv_obj_add_flag(s_card_list, LV_OBJ_FLAG_SCROLLABLE);

    if (s_env->chat_n <= 0 || !s_env->chats) {
        lv_obj_t *empty = lv_label_create(s_card_list);
        nl_ov_set_font(empty);
        lv_label_set_text(empty, "暂无消息");
        lv_obj_set_style_text_color(empty, UI_TEXT_MUTED, 0);
    } else {
        for (int i = 0; i < s_env->chat_n; ++i) {
            const nl_env_chat_msg_t *m = &s_env->chats[i];
            const bool left = strcmp(m->role, "visitor") == 0 || strcmp(m->role, "end_user") == 0;
            support_bubble(s_card_list, m, left);
        }
    }

    if (s_env->chip_n > 0) {
        lv_obj_t *bar = lv_obj_create(s_card_list);
        lv_obj_remove_style_all(bar);
        lv_obj_set_width(bar, lv_pct(100));
        lv_obj_set_height(bar, LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(bar, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
        lv_obj_set_style_pad_row(bar, 8, 0);
        lv_obj_set_style_pad_top(bar, 10, 0);
        lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);

        for (int i = 0; i < s_env->chip_n && i < NL_ENV_CHIP_MAX; ++i) {
            char sentence[NL_ENV_CHAT_TEXT];
            chip_param_text(s_env->chips[i].params_json, sentence, sizeof(sentence));
            lv_obj_t *slot = lv_obj_create(bar);
            lv_obj_remove_style_all(slot);
            lv_obj_set_width(slot, lv_pct(100));
            lv_obj_set_height(slot, LV_SIZE_CONTENT);
            lv_obj_set_flex_flow(slot, LV_FLEX_FLOW_COLUMN);
            lv_obj_set_style_pad_row(slot, 4, 0);
            lv_obj_clear_flag(slot, LV_OBJ_FLAG_SCROLLABLE);
            if (sentence[0]) {
                lv_obj_t *hint = lv_label_create(slot);
                nl_ov_set_font(hint);
                lv_label_set_text(hint, sentence);
                lv_obj_set_width(hint, lv_pct(100));
                lv_label_set_long_mode(hint, LV_LABEL_LONG_WRAP);
                lv_obj_set_style_text_color(hint, UI_TEXT, 0);
            }
            const char *title = s_env->chips[i].title[0] ? s_env->chips[i].title : "执行";
            make_text_action_btn(slot, title, on_support_chip, (void *)(intptr_t)i);
        }
    }

    /* 滚到最新气泡 */
    lv_obj_scroll_to_y(s_card_list, LV_COORD_MAX, LV_ANIM_OFF);
}

bool nodelio_skill_try_support_voice_draft(const char *text)
{
    if (!text || !text[0] || !s_have_env || !s_env) {
        return false;
    }
    if (strcmp(s_env->app_id, "support.inbox") != 0 ||
        strcmp(s_env->card_type, "support_thread") != 0) {
        return false;
    }
    if (!nodelio_skill_result_card_visible()) {
        return false;
    }
    uint64_t cid = 0;
    if (!nodelio_skill_support_conversation_id(&cid) || cid == 0) {
        return false;
    }
    cJSON *body = cJSON_CreateObject();
    if (!body) {
        return false;
    }
    cJSON_AddStringToObject(body, "op", "draft");
    cJSON_AddNumberToObject(body, "conversation_id", (double)cid);
    cJSON_AddStringToObject(body, "text", text);
    char *printed = cJSON_PrintUnformatted(body);
    cJSON_Delete(body);
    if (!printed) {
        return false;
    }
    const bool queued = nl_ov_invoke_skill_async("support.inbox", printed);
    cJSON_free(printed);
    if (queued) {
        nl_ov_toast("正在生成待确认回复");
    }
    return queued;
}
