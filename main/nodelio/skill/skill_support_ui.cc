#include "skill_support_ui.h"
#include "skill_ui_priv.h"

#include "display.h"
#include "nodelio_http.h"
#include "overlay_priv.h"
#include "panel_jobs.h"

#include <esp_heap_caps.h>
#include <esp_jpeg_dec.h>
#include <esp_log.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

LV_FONT_DECLARE(font_noto_emoji_30_4);

static void on_support_chip(lv_event_t *e);
static void on_voice_card(lv_event_t *e);
static void on_voice_cancel(lv_event_t *e);
static void on_voice_again(lv_event_t *e);
static void on_voice_send(lv_event_t *e);

static char s_voice_draft[NL_ENV_CHAT_TEXT];
static uint64_t s_voice_draft_cid = 0;
static bool s_voice_listen = false;
static lv_timer_t *s_auto_tmr = nullptr;
static int s_auto_left = 0;
static int s_auto_idx = -1;
static uint64_t s_auto_cid = 0;
static lv_obj_t *s_auto_tag = nullptr;

static void stop_support_auto(void)
{
    if (s_auto_tmr) {
        lv_timer_del(s_auto_tmr);
        s_auto_tmr = nullptr;
    }
    s_auto_left = 0;
    s_auto_idx = -1;
    s_auto_tag = nullptr;
}

static void support_auto_cb(lv_timer_t *t)
{
    (void)t;
    if (s_auto_left > 1) {
        s_auto_left--;
        if (s_auto_tag) {
            char buf[24];
            snprintf(buf, sizeof(buf), "默认 %d秒", s_auto_left);
            lv_label_set_text(s_auto_tag, buf);
        }
        return;
    }
    const int idx = s_auto_idx;
    stop_support_auto();
    if (s_have_env && s_env && idx >= 0 && idx < s_env->chip_n) {
        run_invoke_action(&s_env->chips[idx]);
    }
}

namespace {

constexpr int kImgSlots = 4;
constexpr int kJpegCap = 384 * 1024;

struct ImgSlot {
    uint8_t *rgb = nullptr;
    lv_image_dsc_t dsc{};
};

struct ImgJob {
    uint32_t gen = 0;
    char url[NL_ENV_URL]{};
    lv_obj_t *img = nullptr;
    lv_obj_t *hint = nullptr;
    int slot = -1;
};

ImgSlot s_imgs[kImgSlots];
uint32_t s_img_gen = 1;
lv_font_t s_chat_font{};

bool text_has_panel_emoji(const char *s)
{
    if (!s) {
        return false;
    }
    const unsigned char *p = reinterpret_cast<const unsigned char *>(s);
    while (*p) {
        uint32_t cp = 0;
        if (*p < 0x80) {
            cp = *p++;
        } else if ((*p & 0xE0) == 0xC0 && p[1]) {
            cp = ((*p & 0x1F) << 6) | (p[1] & 0x3F);
            p += 2;
        } else if ((*p & 0xF0) == 0xE0 && p[1] && p[2]) {
            cp = ((*p & 0x0F) << 12) | ((p[1] & 0x3F) << 6) | (p[2] & 0x3F);
            p += 3;
        } else if ((*p & 0xF8) == 0xF0 && p[1] && p[2] && p[3]) {
            cp = ((*p & 0x07) << 18) | ((p[1] & 0x3F) << 12) | ((p[2] & 0x3F) << 6) | (p[3] & 0x3F);
            p += 4;
        } else {
            ++p;
            continue;
        }
        switch (cp) {
        case 0x1F602:
        case 0x1F606:
        case 0x1F609:
        case 0x1F60C:
        case 0x1F60D:
        case 0x1F60E:
        case 0x1F60F:
        case 0x1F614:
        case 0x1F618:
        case 0x1F61C:
        case 0x1F620:
        case 0x1F62D:
        case 0x1F62F:
        case 0x1F631:
        case 0x1F633:
        case 0x1F634:
        case 0x1F636:
        case 0x1F642:
        case 0x1F644:
        case 0x1F914:
        case 0x1F924:
            return true;
        default:
            break;
        }
    }
    return false;
}

const lv_font_t *chat_font(void)
{
    const lv_font_t *base = nl_ov_ui_font();
    if (!base) {
        return &font_noto_emoji_30_4;
    }
    s_chat_font = *base;
    s_chat_font.fallback = &font_noto_emoji_30_4;
    if (font_noto_emoji_30_4.line_height > s_chat_font.line_height) {
        const int32_t extra = font_noto_emoji_30_4.line_height - s_chat_font.line_height;
        s_chat_font.line_height = font_noto_emoji_30_4.line_height;
        s_chat_font.base_line = static_cast<int32_t>(s_chat_font.base_line) + extra;
    }
    return &s_chat_font;
}

const lv_font_t *font_for(const char *text)
{
    if (text_has_panel_emoji(text)) {
        return chat_font();
    }
    const lv_font_t *base = nl_ov_ui_font();
    return base ? base : &font_noto_emoji_30_4;
}

void free_img_slots(void)
{
    for (int i = 0; i < kImgSlots; ++i) {
        if (s_imgs[i].rgb) {
            heap_caps_free(s_imgs[i].rgb);
            s_imgs[i].rgb = nullptr;
        }
        memset(&s_imgs[i].dsc, 0, sizeof(s_imgs[i].dsc));
    }
}

bool chip_secondary(const char *title)
{
    if (!title || !title[0]) {
        return false;
    }
    return strcmp(title, "返回列表") == 0 || strcmp(title, "不发") == 0 ||
           strstr(title, "取消") != nullptr;
}

bool chip_is_nav(const char *title)
{
    if (!title || !title[0]) {
        return false;
    }
    return strcmp(title, "返回列表") == 0 || strcmp(title, "取消托管") == 0 ||
           strcmp(title, "取消倒计时") == 0 || strcmp(title, "不发") == 0 ||
           strcmp(title, "接管") == 0;
}

int nav_rank(const char *title)
{
    if (!title) {
        return 9;
    }
    if (strcmp(title, "返回列表") == 0) {
        return 0;
    }
    if (strcmp(title, "取消托管") == 0) {
        return 1;
    }
    if (strcmp(title, "取消倒计时") == 0) {
        return 2;
    }
    if (strcmp(title, "不发") == 0) {
        return 3;
    }
    if (strcmp(title, "接管") == 0) {
        return 4;
    }
    return 8;
}

bool caption_is_placeholder(const char *text)
{
    return text && (strcmp(text, "图片") == 0 || strcmp(text, "[图片]") == 0);
}

uint8_t *decode_jpeg_fit(const uint8_t *src, size_t src_len, uint32_t *ow, uint32_t *oh, uint32_t *ostride,
                         size_t *olen)
{
    if (!src || src_len < 4 || src[0] != 0xFF || src[1] != 0xD8) {
        return nullptr;
    }
    jpeg_dec_config_t config = DEFAULT_JPEG_DEC_CONFIG();
    config.output_type = JPEG_PIXEL_FORMAT_RGB565_LE;
    jpeg_dec_handle_t dec = nullptr;
    if (jpeg_dec_open(&config, &dec) != JPEG_ERR_OK) {
        return nullptr;
    }
    jpeg_dec_io_t io{};
    io.inbuf = const_cast<uint8_t *>(src);
    io.inbuf_len = static_cast<int>(src_len);
    jpeg_dec_header_info_t info{};
    if (jpeg_dec_parse_header(dec, &io, &info) != JPEG_ERR_OK || info.width < 8 || info.height < 8) {
        jpeg_dec_close(dec);
        return nullptr;
    }
    int tw = info.width;
    int th = info.height;
    const int min_w = info.width / 8 >= 8 ? info.width / 8 : 8;
    const int min_h = info.height / 8 >= 8 ? info.height / 8 : 8;
    jpeg_dec_close(dec);
    while ((tw > 160 || th > 160) && (tw / 2 >= min_w && th / 2 >= min_h)) {
        tw /= 2;
        th /= 2;
    }
    if (tw < min_w) {
        tw = min_w;
    }
    if (th < min_h) {
        th = min_h;
    }
    tw = (tw / 8) * 8;
    th = (th / 8) * 8;
    if (tw < 8) {
        tw = 8;
    }
    if (th < 8) {
        th = 8;
    }
    if (static_cast<size_t>(tw) * static_cast<size_t>(th) * 2 > 180 * 1024) {
        return nullptr;
    }
    config.scale.width = tw;
    config.scale.height = th;
    if (jpeg_dec_open(&config, &dec) != JPEG_ERR_OK) {
        return nullptr;
    }
    io.inbuf = const_cast<uint8_t *>(src);
    io.inbuf_len = static_cast<int>(src_len);
    if (jpeg_dec_parse_header(dec, &io, &info) != JPEG_ERR_OK) {
        jpeg_dec_close(dec);
        return nullptr;
    }
    const size_t bytes = static_cast<size_t>(tw) * static_cast<size_t>(th) * 2;
    uint8_t *out = static_cast<uint8_t *>(
        heap_caps_aligned_alloc(16, bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!out) {
        out = static_cast<uint8_t *>(heap_caps_aligned_alloc(16, bytes, MALLOC_CAP_8BIT));
    }
    if (!out) {
        jpeg_dec_close(dec);
        return nullptr;
    }
    io.outbuf = out;
    if (jpeg_dec_process(dec, &io) != JPEG_ERR_OK) {
        heap_caps_free(out);
        jpeg_dec_close(dec);
        return nullptr;
    }
    jpeg_dec_close(dec);
    *ow = static_cast<uint32_t>(tw);
    *oh = static_cast<uint32_t>(th);
    *ostride = static_cast<uint32_t>(tw * 2);
    *olen = bytes;
    return out;
}

void img_job_run(void *p)
{
    auto *job = static_cast<ImgJob *>(p);
    uint8_t *jpeg = static_cast<uint8_t *>(
        heap_caps_malloc(kJpegCap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    size_t jpeg_n = 0;
    int status = 0;
    uint8_t *rgb = nullptr;
    uint32_t w = 0, h = 0, stride = 0;
    size_t rgb_len = 0;
    if (jpeg) {
        const esp_err_t err = nodelio_http_get_bin_ms(job->url, nullptr, &status, jpeg, kJpegCap, &jpeg_n, 8000);
        if (err == ESP_OK && status == 200 && jpeg_n > 0) {
            rgb = decode_jpeg_fit(jpeg, jpeg_n, &w, &h, &stride, &rgb_len);
        } else {
            ESP_LOGW("support", "image fetch status=%d err=%d n=%u", status, (int)err, (unsigned)jpeg_n);
        }
    }
    if (jpeg) {
        heap_caps_free(jpeg);
    }

    DisplayLockGuard lock(nl_ov_disp());
    if (!lock || job->gen != s_img_gen || job->slot < 0 || job->slot >= kImgSlots ||
        !job->img || !lv_obj_is_valid(job->img)) {
        if (rgb) {
            heap_caps_free(rgb);
        }
        delete job;
        return;
    }
    if (!rgb) {
        if (job->hint && lv_obj_is_valid(job->hint)) {
            lv_label_set_text(job->hint, "图片无法显示");
        }
        delete job;
        return;
    }
    if (s_imgs[job->slot].rgb) {
        heap_caps_free(s_imgs[job->slot].rgb);
    }
    s_imgs[job->slot].rgb = rgb;
    lv_image_dsc_t *dsc = &s_imgs[job->slot].dsc;
    memset(dsc, 0, sizeof(*dsc));
    dsc->header.magic = LV_IMAGE_HEADER_MAGIC;
    dsc->header.cf = LV_COLOR_FORMAT_RGB565;
    dsc->header.w = w;
    dsc->header.h = h;
    dsc->header.stride = stride;
    dsc->data_size = static_cast<uint32_t>(rgb_len);
    dsc->data = rgb;
    lv_image_set_src(job->img, dsc);
    lv_obj_clear_flag(job->img, LV_OBJ_FLAG_HIDDEN);
    if (job->hint && lv_obj_is_valid(job->hint)) {
        lv_obj_add_flag(job->hint, LV_OBJ_FLAG_HIDDEN);
    }
    delete job;
}

void queue_image(int slot, const char *url, lv_obj_t *img, lv_obj_t *hint)
{
    if (slot < 0 || slot >= kImgSlots || !url || strncmp(url, "http", 4) != 0) {
        if (hint) {
            lv_label_set_text(hint, "图片");
        }
        return;
    }
    auto *job = new ImgJob();
    job->gen = s_img_gen;
    snprintf(job->url, sizeof(job->url), "%s", url);
    job->img = img;
    job->hint = hint;
    job->slot = slot;
    if (!nodelio::SubmitPanelJob(nodelio::PanelJobLane::Background, img_job_run, job,
                                 [](void *p) { delete static_cast<ImgJob *>(p); })) {
        delete job;
        if (hint) {
            lv_label_set_text(hint, "图片");
        }
    }
}

void chip_param_text(const char *params_json, char *out, size_t out_len)
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

lv_obj_t *support_bubble(lv_obj_t *parent, const nl_env_chat_msg_t *m, bool left, int *img_slot)
{
    const bool mine = !left && strcmp(m->role, "system") != 0;
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_remove_style_all(row);
    lv_obj_set_width(row, lv_pct(100));
    lv_obj_set_height(row, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    /* 时间贴在气泡下沿，避免单独成行被读成下一条的标题 */
    lv_obj_set_flex_align(row, left ? LV_FLEX_ALIGN_START : LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END,
                          LV_FLEX_ALIGN_END);
    lv_obj_set_style_pad_ver(row, 3, 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *col = lv_obj_create(row);
    lv_obj_remove_style_all(col);
    lv_obj_set_width(col, LV_SIZE_CONTENT);
    lv_obj_set_height(col, LV_SIZE_CONTENT);
    lv_obj_set_flex_grow(col, 0);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(col, LV_FLEX_ALIGN_START, left ? LV_FLEX_ALIGN_START : LV_FLEX_ALIGN_END,
                          LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(col, 2, 0);
    lv_obj_clear_flag(col, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(col, LV_OBJ_FLAG_CLICKABLE);

    if (mine && m->name[0]) {
        lv_obj_t *name = lv_label_create(col);
        nl_ov_set_font(name);
        lv_label_set_text(name, m->name);
        lv_obj_set_style_text_color(name, UI_TEXT_MUTED, 0);
    }

    lv_obj_t *bubble = lv_obj_create(col);
    lv_obj_remove_style_all(bubble);
    lv_obj_set_width(bubble, LV_SIZE_CONTENT);
    lv_obj_set_height(bubble, LV_SIZE_CONTENT);
    if (strcmp(m->role, "system") == 0) {
        lv_obj_set_style_bg_opa(bubble, LV_OPA_TRANSP, 0);
        lv_obj_set_style_pad_all(bubble, 0, 0);
    } else {
        const bool bot = strcmp(m->role, "bot") == 0;
        lv_obj_set_style_bg_color(bubble, left ? UI_SURFACE_PRESSED : (bot ? UI_INFO : UI_ACCENT), 0);
        lv_obj_set_style_bg_opa(bubble, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(bubble, 16, 0);
        lv_obj_set_style_pad_hor(bubble, 12, 0);
        lv_obj_set_style_pad_ver(bubble, 8, 0);
        lv_obj_set_style_pad_row(bubble, 6, 0);
    }
    lv_obj_set_flex_flow(bubble, LV_FLEX_FLOW_COLUMN);
    lv_obj_clear_flag(bubble, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(bubble, LV_OBJ_FLAG_CLICKABLE);

    if (m->image_url[0]) {
        lv_obj_t *hint = lv_label_create(bubble);
        nl_ov_set_font(hint);
        lv_label_set_text(hint, "图片加载中");
        lv_obj_set_style_text_color(hint, left ? UI_TEXT_MUTED : UI_ON_TILE, 0);
        lv_obj_t *img = lv_image_create(bubble);
        lv_obj_add_flag(img, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_style_radius(img, 10, 0);
        if (img_slot && *img_slot < kImgSlots) {
            queue_image(*img_slot, m->image_url, img, hint);
            *img_slot += 1;
        } else {
            lv_label_set_text(hint, "图片");
        }
    }

    if (m->text[0] && !(m->image_url[0] && caption_is_placeholder(m->text))) {
        lv_obj_t *body = lv_label_create(bubble);
        const lv_font_t *font = font_for(m->text);
        lv_obj_set_style_text_font(body, font, 0);
        lv_label_set_text(body, m->text);
        lv_obj_set_style_text_color(body, left || strcmp(m->role, "system") == 0 ? UI_TEXT : UI_ON_TILE, 0);
        const int max_w = skill_panel_w() > 40 ? skill_panel_w() * 68 / 100 : 160;
        lv_point_t sz{};
        lv_text_get_size(&sz, m->text, font, 0, 0, max_w, LV_TEXT_FLAG_NONE);
        lv_obj_set_width(body, sz.x > 8 ? sz.x : 8);
        lv_label_set_long_mode(body, LV_LABEL_LONG_WRAP);
    }

    if (m->sent_at[0] && strcmp(m->role, "system") != 0) {
        lv_obj_t *when = lv_label_create(row);
        nl_ov_set_font_compact(when);
        lv_label_set_text(when, m->sent_at);
        lv_obj_set_style_text_color(when, UI_TEXT_MUTED, 0);
        lv_obj_set_style_pad_hor(when, 6, 0);
        lv_obj_set_style_pad_bottom(when, 4, 0);
        lv_obj_clear_flag(when, LV_OBJ_FLAG_CLICKABLE);
        if (!left) {
            lv_obj_move_to_index(when, lv_obj_get_index(col));
        }
    }
    return row;
}

const lv_font_t *suggest_font(void)
{
    const lv_font_t *font = nl_ov_ui_font_compact();
    return font ? font : font_for("");
}

int suggest_card_h(int body_h)
{
    const int line_h = lv_font_get_line_height(suggest_font());
    if (body_h < line_h) {
        body_h = line_h;
    }
    /* 正文按最长一条撑开，各卡同高，角标行始终留出 */
    return 8 + body_h + 4 + line_h + 8;
}

int suggest_body_h(const char *sentence, int text_w)
{
    const lv_font_t *font = suggest_font();
    const int line_h = lv_font_get_line_height(font);
    if (!sentence || !sentence[0] || text_w < 8) {
        return line_h;
    }
    lv_point_t sz{};
    lv_text_get_size(&sz, sentence, font, 0, 0, text_w, LV_TEXT_FLAG_NONE);
    if (sz.y < line_h) {
        return line_h;
    }
    return sz.y;
}

void support_suggest_card(lv_obj_t *parent, int index, int visual, const char *sentence, const char *title,
                          bool voice_slot, int body_h, int text_w)
{
    const lv_font_t *font = suggest_font();
    const int line_h = lv_font_get_line_height(font);
    if (body_h < line_h) {
        body_h = line_h;
    }
    const lv_color_t fills[] = {UI_TILE_SWITCH, UI_TILE_PWM, UI_TILE_SCENE, UI_TILE_GPIO};
    const lv_color_t fill = fills[visual < 0 ? 0 : (visual % 4)];

    lv_obj_t *card = lv_obj_create(parent);
    lv_obj_remove_style_all(card);
    lv_obj_set_height(card, suggest_card_h(body_h));
    lv_obj_set_flex_grow(card, 1);
    lv_obj_set_style_min_width(card, 0, 0);
    lv_obj_set_width(card, lv_pct(32));
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(card, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_all(card, 8, 0);
    lv_obj_set_style_pad_row(card, 4, 0);
    lv_obj_set_style_radius(card, 12, 0);
    lv_obj_set_style_bg_color(card, fill, 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(card, UI_SURFACE_PRESSED, LV_STATE_PRESSED);
    lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(card, voice_slot ? on_voice_card : on_support_chip, LV_EVENT_CLICKED,
                        (void *)(intptr_t)index);

    lv_obj_t *hint = lv_label_create(card);
    lv_obj_set_style_text_font(hint, font, 0);
    lv_label_set_text(hint, sentence);
    lv_obj_set_width(hint, text_w > 8 ? text_w : lv_pct(100));
    lv_obj_set_height(hint, body_h);
    lv_label_set_long_mode(hint, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_color(hint, UI_TEXT, 0);
    lv_obj_clear_flag(hint, LV_OBJ_FLAG_CLICKABLE);

    const bool is_default = title && strncmp(title, "默认", sizeof("默认") - 1) == 0;
    const bool counting = s_auto_left > 0 && index == s_auto_idx;
    if (counting || is_default || (title && title[0] && strcmp(title, "发送") != 0)) {
        lv_obj_t *tag = lv_label_create(card);
        lv_obj_set_style_text_font(tag, font, 0);
        if (counting) {
            char buf[24];
            snprintf(buf, sizeof(buf), "默认 %d秒", s_auto_left);
            lv_label_set_text(tag, buf);
            s_auto_tag = tag;
        } else {
            lv_label_set_text(tag, is_default ? "默认" : title);
        }
        lv_obj_set_style_text_color(tag, UI_ACCENT, 0);
        lv_obj_clear_flag(tag, LV_OBJ_FLAG_CLICKABLE);
    }
}

void voice_text_btn(lv_obj_t *parent, const char *title, lv_event_cb_t cb, lv_color_t fg)
{
    const int h = lv_font_get_line_height(suggest_font()) + 8;
    lv_obj_t *btn = lv_obj_create(parent);
    lv_obj_remove_style_all(btn);
    lv_obj_set_height(btn, h);
    lv_obj_set_flex_grow(btn, 1);
    lv_obj_set_style_min_width(btn, 0, 0);
    lv_obj_set_width(btn, 0);
    lv_obj_set_style_radius(btn, 8, 0);
    lv_obj_set_style_bg_color(btn, UI_CAPSULE_OFF, 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(btn, UI_SURFACE_PRESSED, LV_STATE_PRESSED);
    lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, nullptr);
    lv_obj_t *lbl = lv_label_create(btn);
    lv_obj_set_style_text_font(lbl, suggest_font(), 0);
    lv_label_set_text(lbl, title);
    lv_obj_set_style_text_color(lbl, fg, 0);
    lv_obj_center(lbl);
    lv_obj_clear_flag(lbl, LV_OBJ_FLAG_CLICKABLE);
}

void support_voice_card(lv_obj_t *parent, int body_h, int text_w)
{
    const bool filled = s_voice_draft[0] != '\0';
    const bool listening = s_voice_listen && !filled;
    const char *sentence = filled ? s_voice_draft : (listening ? "请说要发的话" : "自己说一句");
    const lv_font_t *font = suggest_font();
    const int line_h = lv_font_get_line_height(font);
    if (body_h < line_h) {
        body_h = line_h;
    }

    lv_obj_t *card = lv_obj_create(parent);
    lv_obj_remove_style_all(card);
    lv_obj_set_height(card, suggest_card_h(body_h) + (filled ? 12 : 0));
    lv_obj_set_flex_grow(card, 1);
    lv_obj_set_style_min_width(card, 0, 0);
    lv_obj_set_width(card, lv_pct(24));
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(card, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_all(card, 8, 0);
    lv_obj_set_style_pad_row(card, 4, 0);
    lv_obj_set_style_radius(card, 12, 0);
    lv_obj_set_style_bg_color(card, UI_TILE_GPIO, 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(card, UI_SURFACE_PRESSED, LV_STATE_PRESSED);
    lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    if (!filled) {
        lv_obj_add_event_cb(card, on_voice_card, LV_EVENT_CLICKED, nullptr);
    }

    lv_obj_t *hint = lv_label_create(card);
    lv_obj_set_style_text_font(hint, font, 0);
    lv_label_set_text(hint, sentence);
    lv_obj_set_width(hint, text_w > 8 ? text_w : lv_pct(100));
    lv_obj_set_height(hint, body_h);
    lv_label_set_long_mode(hint, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_color(hint, (filled || listening) ? UI_TEXT : UI_TEXT_MUTED, 0);
    lv_obj_clear_flag(hint, LV_OBJ_FLAG_CLICKABLE);

    if (!filled) {
        lv_obj_t *tag = lv_label_create(card);
        lv_obj_set_style_text_font(tag, font, 0);
        lv_label_set_text(tag, listening ? "正在听" : "直接说");
        lv_obj_set_style_text_color(tag, UI_WARN, 0);
        lv_obj_clear_flag(tag, LV_OBJ_FLAG_CLICKABLE);
        return;
    }

    lv_obj_t *row = lv_obj_create(card);
    lv_obj_remove_style_all(row);
    lv_obj_set_width(row, lv_pct(100));
    lv_obj_set_height(row, line_h + 10);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, 6, 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    voice_text_btn(row, "取消", on_voice_cancel, UI_TEXT_SUB);
    voice_text_btn(row, "重新说", on_voice_again, UI_WARN);
    voice_text_btn(row, "发送", on_voice_send, UI_ACCENT);
}

void ensure_release_chip(void)
{
    if (!s_env || !strstr(s_env->display_title, "已接管") || s_env->chip_n >= NL_ENV_CHIP_MAX) {
        return;
    }
    for (int i = 0; i < s_env->chip_n; ++i) {
        if (strcmp(s_env->chips[i].title, "取消托管") == 0) {
            return;
        }
    }
    uint64_t cid = s_env->conversation_id;
    if (cid == 0) {
        for (int i = 0; i < s_env->chip_n && cid == 0; ++i) {
            cJSON *root = cJSON_Parse(s_env->chips[i].params_json);
            const cJSON *jc = root ? cJSON_GetObjectItemCaseSensitive(root, "conversation_id") : nullptr;
            if (cJSON_IsNumber(jc) && jc->valuedouble > 0) {
                cid = (uint64_t)jc->valuedouble;
            }
            cJSON_Delete(root);
        }
    }
    if (cid == 0) {
        return;
    }
    nl_env_invoke_action_t *ia = &s_env->chips[s_env->chip_n];
    memset(ia, 0, sizeof(*ia));
    snprintf(ia->id, sizeof(ia->id), "release");
    snprintf(ia->title, sizeof(ia->title), "取消托管");
    snprintf(ia->app_id, sizeof(ia->app_id), "support.inbox");
    snprintf(ia->params_json, sizeof(ia->params_json),
             "{\"op\":\"release\",\"conversation_id\":%llu}", (unsigned long long)cid);
    s_env->chip_n += 1;
}

void fill_support_nav(void)
{
    if (!s_support_nav || !s_env) {
        return;
    }
    ensure_release_chip();
    lv_obj_clean(s_support_nav);
    if (s_filter_bar) {
        lv_obj_add_flag(s_filter_bar, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_height(s_filter_bar, 0);
    }
    int order[NL_ENV_CHIP_MAX];
    int n = 0;
    for (int i = 0; i < s_env->chip_n && i < NL_ENV_CHIP_MAX; ++i) {
        const char *title = s_env->chips[i].title;
        if (!chip_is_nav(title)) {
            continue;
        }
        int at = n;
        while (at > 0 && nav_rank(s_env->chips[order[at - 1]].title) > nav_rank(title)) {
            order[at] = order[at - 1];
            --at;
        }
        order[at] = i;
        ++n;
    }
    if (n <= 0) {
        hide_support_title_nav();
        return;
    }
    lv_obj_clear_flag(s_support_nav, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_width(s_support_nav, LV_SIZE_CONTENT);
    lv_obj_set_height(s_support_nav, LV_SIZE_CONTENT);
    if (s_card_title) {
        lv_obj_set_width(s_card_title, LV_SIZE_CONTENT);
        lv_obj_set_flex_grow(s_card_title, 0);
        int cap = skill_panel_w() / 2;
        if (cap < 96) {
            cap = 96;
        }
        lv_obj_set_style_max_width(s_card_title, cap, 0);
    }
    for (int k = 0; k < n; ++k) {
        const int i = order[k];
        const char *title = s_env->chips[i].title;
        lv_obj_t *btn = make_text_action_btn(s_support_nav, title, on_support_chip, (void *)(intptr_t)i);
        if (!btn) {
            continue;
        }
        lv_obj_set_height(btn, 36);
        lv_obj_set_width(btn, LV_SIZE_CONTENT);
        lv_obj_set_flex_grow(btn, 0);
        if (strcmp(title, "取消托管") == 0) {
            lv_obj_set_style_bg_color(btn, UI_DANGER, 0);
            lv_obj_t *lbl = lv_obj_get_child(btn, 0);
            if (lbl) {
                lv_obj_set_style_text_color(lbl, UI_ON_TILE, 0);
            }
        } else if (chip_secondary(title)) {
            lv_obj_set_style_bg_color(btn, UI_CAPSULE_OFF, 0);
            lv_obj_t *lbl = lv_obj_get_child(btn, 0);
            if (lbl) {
                lv_obj_set_style_text_color(lbl, UI_TEXT_SUB, 0);
            }
        }
    }
}

void fill_action_bar(void)
{
    if (!s_dsh_detail_bar) {
        return;
    }
    s_auto_tag = nullptr;
    lv_obj_clean(s_dsh_detail_bar);
    if (!s_env || s_env->chip_n <= 0) {
        stop_support_auto();
        lv_obj_add_flag(s_dsh_detail_bar, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_height(s_dsh_detail_bar, 0);
        return;
    }
    const bool support = strcmp(s_env->card_type, "support_thread") == 0;
    int default_idx = -1;
    bool server_auto = false;
    int ncard = support ? 1 : 0;
    if (support) {
        for (int i = 0; i < s_env->chip_n && i < NL_ENV_CHIP_MAX; ++i) {
            const char *title = s_env->chips[i].title;
            if (chip_is_nav(title)) {
                if (strcmp(title, "取消倒计时") == 0) {
                    server_auto = true;
                }
                continue;
            }
            char sentence[NL_ENV_CHAT_TEXT];
            chip_param_text(s_env->chips[i].params_json, sentence, sizeof(sentence));
            if (!sentence[0]) {
                continue;
            }
            ncard++;
            if (default_idx < 0) {
                default_idx = i;
            }
        }
    }
    if (ncard < 1) {
        ncard = 1;
    }
    int row_w = skill_panel_w() - 24;
    if (row_w < 80) {
        row_w = 80;
    }
    int text_w = (row_w - 6 * (ncard - 1)) / ncard - 16;
    if (text_w < 48) {
        text_w = 48;
    }
    const char *voice = s_voice_draft[0] ? s_voice_draft : (s_voice_listen ? "请说要发的话" : "自己说一句");
    int body_h = support ? suggest_body_h(voice, text_w) : suggest_body_h("", text_w);
    if (support) {
        for (int i = 0; i < s_env->chip_n && i < NL_ENV_CHIP_MAX; ++i) {
            if (chip_is_nav(s_env->chips[i].title)) {
                continue;
            }
            char sentence[NL_ENV_CHAT_TEXT];
            chip_param_text(s_env->chips[i].params_json, sentence, sizeof(sentence));
            if (!sentence[0]) {
                continue;
            }
            const int h = suggest_body_h(sentence, text_w);
            if (h > body_h) {
                body_h = h;
            }
        }
    }
    const bool start_local = support && default_idx >= 0 && !server_auto;
    const bool keep = start_local && s_auto_tmr && s_auto_idx == default_idx &&
                      s_auto_cid == s_env->conversation_id;
    if (start_local) {
        if (!keep) {
            if (s_auto_tmr) {
                lv_timer_del(s_auto_tmr);
                s_auto_tmr = nullptr;
            }
            s_auto_idx = default_idx;
            s_auto_left = 20;
            s_auto_cid = s_env->conversation_id;
            s_auto_tmr = lv_timer_create(support_auto_cb, 1000, nullptr);
        }
    } else {
        stop_support_auto();
    }
    if (support) {
        fill_support_nav();
    }
    lv_obj_set_flex_flow(s_dsh_detail_bar, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_dsh_detail_bar, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(s_dsh_detail_bar, 6, 0);
    lv_obj_set_style_pad_top(s_dsh_detail_bar, 6, 0);
    lv_obj_set_height(s_dsh_detail_bar, LV_SIZE_CONTENT);
    if (!support) {
        lv_obj_set_style_max_height(s_dsh_detail_bar, 148, 0);
    }
    lv_obj_clear_flag(s_dsh_detail_bar, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(s_dsh_detail_bar, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t *suggest = nullptr;
    lv_obj_t *bare_row = nullptr;
    for (int i = 0; i < s_env->chip_n && i < NL_ENV_CHIP_MAX; ++i) {
        char sentence[NL_ENV_CHAT_TEXT];
        chip_param_text(s_env->chips[i].params_json, sentence, sizeof(sentence));
        const char *title = s_env->chips[i].title[0] ? s_env->chips[i].title : "执行";
        if (support && chip_is_nav(title)) {
            continue;
        }
        if (support && sentence[0]) {
            const int visual = suggest ? (int)lv_obj_get_child_count(suggest) : 0;
            if (!suggest) {
                suggest = lv_obj_create(s_dsh_detail_bar);
                lv_obj_remove_style_all(suggest);
                lv_obj_set_width(suggest, lv_pct(100));
                lv_obj_set_height(suggest, LV_SIZE_CONTENT);
                lv_obj_set_flex_flow(suggest, LV_FLEX_FLOW_ROW);
                lv_obj_set_flex_align(suggest, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
                lv_obj_set_style_pad_column(suggest, 6, 0);
                lv_obj_clear_flag(suggest, LV_OBJ_FLAG_SCROLLABLE);
            }
            support_suggest_card(suggest, i, visual, sentence, title, false, body_h, text_w);
            continue;
        }
        if (!bare_row) {
            bare_row = lv_obj_create(s_dsh_detail_bar);
            lv_obj_remove_style_all(bare_row);
            lv_obj_set_width(bare_row, lv_pct(100));
            lv_obj_set_height(bare_row, 44);
            lv_obj_set_flex_flow(bare_row, LV_FLEX_FLOW_ROW);
            lv_obj_set_flex_align(bare_row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
            lv_obj_set_style_pad_column(bare_row, 8, 0);
            lv_obj_clear_flag(bare_row, LV_OBJ_FLAG_SCROLLABLE);
        }
        lv_obj_t *btn = make_text_action_btn(bare_row, title, on_support_chip, (void *)(intptr_t)i);
        if (!btn) {
            continue;
        }
        lv_obj_set_height(btn, 44);
        lv_obj_set_width(btn, 48);
        lv_obj_set_flex_grow(btn, 1);
        lv_obj_set_style_min_width(btn, 0, 0);
        if (chip_secondary(title)) {
            lv_obj_set_style_bg_color(btn, UI_CAPSULE_OFF, 0);
            lv_obj_t *lbl = lv_obj_get_child(btn, 0);
            if (lbl) {
                lv_obj_set_style_text_color(lbl, UI_TEXT_SUB, 0);
            }
        }
    }
    if (support) {
        if (!suggest) {
            suggest = lv_obj_create(s_dsh_detail_bar);
            lv_obj_remove_style_all(suggest);
            lv_obj_set_width(suggest, lv_pct(100));
            lv_obj_set_height(suggest, LV_SIZE_CONTENT);
            lv_obj_set_flex_flow(suggest, LV_FLEX_FLOW_ROW);
            lv_obj_set_flex_align(suggest, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
            lv_obj_set_style_pad_column(suggest, 6, 0);
            lv_obj_clear_flag(suggest, LV_OBJ_FLAG_SCROLLABLE);
        }
        support_voice_card(suggest, body_h, text_w);
    }
    if (support && !suggest && !bare_row) {
        lv_obj_add_flag(s_dsh_detail_bar, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_height(s_dsh_detail_bar, 0);
    }
}

}  // namespace

void hide_support_title_nav(void)
{
    if (s_support_nav) {
        lv_obj_clean(s_support_nav);
        lv_obj_add_flag(s_support_nav, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_width(s_support_nav, 0);
        lv_obj_set_height(s_support_nav, 0);
    }
    if (s_card_title) {
        lv_obj_set_flex_grow(s_card_title, 1);
        lv_obj_set_width(s_card_title, lv_pct(100));
        lv_obj_set_style_max_width(s_card_title, LV_COORD_MAX, 0);
    }
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
    stop_support_auto();
    run_invoke_action(&s_env->chips[idx]);
}

static void voice_start_listen(void)
{
    s_voice_draft[0] = '\0';
    s_voice_listen = true;
    if (s_env) {
        s_voice_draft_cid = s_env->conversation_id;
    }
    fill_action_bar();
    Board::GetInstance().NoteUserActivity();
    restart_skill_ttl();
    {
        DisplayLockGuard lock(nl_ov_disp());
        if (lock) {
            nl_ov_ai_drawer_force_listen();
            nodelio_skill_result_raise();
        }
    }
    Application::GetInstance().Schedule([]() {
        nodelio_offline_cmd_yield_for_cloud_audio();
        auto &app = Application::GetInstance();
        const auto st = app.GetDeviceState();
        /* 自动停听：说完云端直接回识别字。手动停听要再发 listen stop 才会出字。 */
        if (st == kDeviceStateIdle || st == kDeviceStateNotifying || st == kDeviceStateSpeaking) {
            app.StartSupportReplyListen();
            return;
        }
        if (st != kDeviceStateListening && st != kDeviceStateConnecting) {
            nodelio_skill_support_voice_fail("现在开不了麦");
        }
    });
    nl_ov_toast("正在听，说完会出现在这里");
}

static void on_voice_cancel(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) {
        return;
    }
    s_voice_draft[0] = '\0';
    s_voice_listen = false;
    fill_action_bar();
}

static void on_voice_again(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) {
        return;
    }
    voice_start_listen();
}

static void on_voice_card(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED || s_voice_draft[0]) {
        return;
    }
    if (!s_voice_listen) {
        voice_start_listen();
        return;
    }
    nl_ov_toast("正在听，说完会出现在这里");
}

static void on_voice_send(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED || !s_voice_draft[0]) {
        return;
    }
    s_voice_listen = false;
    uint64_t cid = s_voice_draft_cid;
    if (cid == 0 && s_env) {
        cid = s_env->conversation_id;
    }
    if (cid == 0) {
        nl_ov_toast("还没有打开会话");
        return;
    }
    cJSON *obj = cJSON_CreateObject();
    if (!obj) {
        return;
    }
    cJSON_AddStringToObject(obj, "op", "reply");
    cJSON_AddNumberToObject(obj, "conversation_id", (double)cid);
    cJSON_AddStringToObject(obj, "text", s_voice_draft);
    char *printed = cJSON_PrintUnformatted(obj);
    cJSON_Delete(obj);
    if (!printed || strlen(printed) >= NL_ENV_PARAMS) {
        cJSON_free(printed);
        nl_ov_toast("这句话太长");
        return;
    }
    nl_env_invoke_action_t act{};
    snprintf(act.app_id, sizeof(act.app_id), "support.inbox");
    snprintf(act.title, sizeof(act.title), "发送");
    snprintf(act.params_json, sizeof(act.params_json), "%s", printed);
    cJSON_free(printed);
    s_voice_draft[0] = '\0';
    stop_support_auto();
    run_invoke_action(&act);
}

void fill_support_thread(void)
{
    if (!s_card_list || !s_env) {
        return;
    }
    s_img_gen += 1;
    free_img_slots();
    if (s_env->conversation_id != s_voice_draft_cid) {
        s_voice_draft[0] = '\0';
        s_voice_draft_cid = s_env->conversation_id;
        s_voice_listen = false;
    }
    lv_obj_clean(s_card_list);
    lv_obj_set_flex_flow(s_card_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_card_list, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(s_card_list, 2, 0);
    lv_obj_set_style_pad_hor(s_card_list, 2, 0);
    lv_obj_set_style_pad_bottom(s_card_list, 8, 0);
    lv_obj_set_scroll_dir(s_card_list, LV_DIR_VER);
    lv_obj_add_flag(s_card_list, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(s_card_list, LV_SCROLLBAR_MODE_OFF);

    if (s_card_sub) {
        lv_obj_set_style_text_color(s_card_sub, UI_TEXT_MUTED, 0);
        /* 来源只留一行。时间在每条气泡旁，不再在顶栏重复。 */
        lv_label_set_long_mode(s_card_sub, LV_LABEL_LONG_DOT);
        lv_obj_set_height(s_card_sub, nl_ov_ui_font_line_h());
        if (s_env->support_meta[0]) {
            char line[192];
            size_t w = 0;
            for (const char *p = s_env->support_meta; *p && w + 1 < sizeof(line); ++p) {
                line[w++] = (*p == '\n') ? ' ' : *p;
            }
            line[w] = '\0';
            lv_label_set_text(s_card_sub, line);
        } else if (s_env->chat_n <= 0) {
            lv_label_set_text(s_card_sub, "暂无消息");
        } else {
            lv_label_set_text_fmt(s_card_sub, "%d 条消息", s_env->chat_n);
        }
    }

    if (s_env->chat_n <= 0 || !s_env->chats) {
        lv_obj_t *empty = lv_label_create(s_card_list);
        nl_ov_set_font(empty);
        lv_label_set_text(empty, "暂无消息");
        lv_obj_set_style_text_color(empty, UI_TEXT_MUTED, 0);
    } else {
        int img_slot = 0;
        for (int i = 0; i < s_env->chat_n; ++i) {
            const nl_env_chat_msg_t *m = &s_env->chats[i];
            const bool left = strcmp(m->role, "visitor") == 0 || strcmp(m->role, "end_user") == 0 ||
                              strcmp(m->role, "system") == 0;
            support_bubble(s_card_list, m, left, &img_slot);
        }
    }
    fill_action_bar();
}

bool nodelio_skill_support_keep_thread(const char *invoke_params, char *out, size_t out_len)
{
    if (!out || out_len < 16) {
        return false;
    }
    out[0] = '\0';
    if (!invoke_params || !invoke_params[0] || !nodelio_skill_result_card_visible() || !s_have_env ||
        !s_env || strcmp(s_env->card_type, "support_thread") != 0) {
        return false;
    }
    uint64_t open_id = 0;
    if (!nodelio_skill_support_conversation_id(&open_id) || open_id == 0) {
        return false;
    }
    cJSON *root = cJSON_Parse(invoke_params);
    if (!root || !cJSON_IsObject(root)) {
        cJSON_Delete(root);
        return false;
    }
    const cJSON *op = cJSON_GetObjectItemCaseSensitive(root, "op");
    if (cJSON_IsString(op) && op->valuestring && op->valuestring[0] &&
        strcmp(op->valuestring, "inbox") != 0) {
        cJSON_Delete(root);
        return false;
    }
    const cJSON *jc = cJSON_GetObjectItemCaseSensitive(root, "conversation_id");
    uint64_t cid = 0;
    if (cJSON_IsNumber(jc) && jc->valuedouble > 0) {
        cid = (uint64_t)jc->valuedouble;
    } else if (cJSON_IsString(jc) && jc->valuestring && jc->valuestring[0]) {
        cid = (uint64_t)strtoull(jc->valuestring, nullptr, 10);
    }
    if (cid == 0 || cid != open_id) {
        cJSON_Delete(root);
        return false;
    }
    cJSON *repl = cJSON_CreateString("thread");
    if (!repl) {
        cJSON_Delete(root);
        return false;
    }
    if (cJSON_GetObjectItemCaseSensitive(root, "op")) {
        cJSON_ReplaceItemInObject(root, "op", repl);
    } else {
        cJSON_AddItemToObject(root, "op", repl);
    }
    char *printed = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!printed) {
        return false;
    }
    const size_t n = strlen(printed);
    if (n + 1 > out_len) {
        cJSON_free(printed);
        return false;
    }
    memcpy(out, printed, n + 1);
    cJSON_free(printed);
    return true;
}

bool nodelio_skill_support_thread_open(void)
{
    DisplayLockGuard lock(nl_ov_disp());
    if (!lock) {
        return false;
    }
    return s_have_env && s_env && nodelio_skill_result_card_visible() &&
           strcmp(s_env->card_type, "support_thread") == 0;
}

bool nodelio_skill_support_visitor_text(char *out, size_t out_len)
{
    if (!out || out_len == 0) {
        return false;
    }
    out[0] = '\0';
    DisplayLockGuard lock(nl_ov_disp());
    if (!lock || !s_have_env || !s_env || strcmp(s_env->card_type, "support_thread") != 0) {
        return false;
    }
    if (s_env->chats && s_env->chat_n > 0) {
        for (int i = s_env->chat_n - 1; i >= 0; --i) {
            const nl_env_chat_msg_t *m = &s_env->chats[i];
            if ((strcmp(m->role, "visitor") == 0 || strcmp(m->role, "end_user") == 0) && m->text[0]) {
                snprintf(out, out_len, "%s", m->text);
                return out[0] != '\0';
            }
        }
    }
    if (s_env->spoken[0]) {
        snprintf(out, out_len, "%s", s_env->spoken);
        return true;
    }
    return false;
}

static bool fit_reply_params(char *out, size_t out_len, uint64_t cid, uint64_t mid, const char *text)
{
    char body[NL_ENV_CHAT_TEXT];
    snprintf(body, sizeof(body), "%s", text ? text : "");
    while (body[0]) {
        cJSON *obj = cJSON_CreateObject();
        if (!obj) {
            return false;
        }
        cJSON_AddStringToObject(obj, "op", "reply");
        cJSON_AddNumberToObject(obj, "conversation_id", (double)cid);
        if (mid > 0) {
            cJSON_AddNumberToObject(obj, "message_id", (double)mid);
        }
        cJSON_AddStringToObject(obj, "text", body);
        char *printed = cJSON_PrintUnformatted(obj);
        cJSON_Delete(obj);
        if (!printed) {
            return false;
        }
        const size_t n = strlen(printed);
        if (n + 1 <= out_len) {
            memcpy(out, printed, n + 1);
            cJSON_free(printed);
            return true;
        }
        cJSON_free(printed);
        size_t len = strlen(body);
        if (len == 0) {
            return false;
        }
        do {
            len--;
        } while (len > 0 && (static_cast<unsigned char>(body[len]) & 0xC0) == 0x80);
        body[len] = '\0';
    }
    return false;
}

bool nodelio_skill_support_apply_cloud_replies(const char *const *replies, int n)
{
    if (!replies || n <= 0) {
        return false;
    }
    DisplayLockGuard lock(nl_ov_disp());
    if (!lock || !s_have_env || !s_env || !s_env->cloud_suggest ||
        strcmp(s_env->card_type, "support_thread") != 0 || !nodelio_skill_result_card_visible()) {
        return false;
    }
    nl_env_invoke_action_t kept[NL_ENV_CHIP_MAX];
    int kept_n = 0;
    for (int i = 0; i < s_env->chip_n && i < NL_ENV_CHIP_MAX; ++i) {
        const char *id = s_env->chips[i].id;
        if (strcmp(id, "suggest_fail") == 0 || strncmp(id, "reply_", 6) == 0) {
            continue;
        }
        kept[kept_n++] = s_env->chips[i];
    }
    int use = n > 4 ? 4 : n;
    if (use + kept_n > NL_ENV_CHIP_MAX) {
        use = NL_ENV_CHIP_MAX - kept_n;
    }
    if (use <= 0) {
        return false;
    }
    const uint64_t cid = s_env->conversation_id;
    const uint64_t mid = s_env->message_id;
    nl_env_invoke_action_t next[NL_ENV_CHIP_MAX];
    int w = 0;
    for (int i = 0; i < use && w < NL_ENV_CHIP_MAX; ++i) {
        if (!replies[i] || !replies[i][0]) {
            continue;
        }
        nl_env_invoke_action_t *ia = &next[w];
        memset(ia, 0, sizeof(*ia));
        snprintf(ia->id, sizeof(ia->id), "reply_%d", w + 1);
        snprintf(ia->title, sizeof(ia->title), "%s", w == 0 ? "默认" : "发送");
        snprintf(ia->app_id, sizeof(ia->app_id), "support.inbox");
        if (!fit_reply_params(ia->params_json, sizeof(ia->params_json), cid, mid, replies[i])) {
            continue;
        }
        w++;
    }
    if (w <= 0) {
        return false;
    }
    for (int i = 0; i < kept_n && w < NL_ENV_CHIP_MAX; ++i) {
        next[w++] = kept[i];
    }
    for (int i = 0; i < w; ++i) {
        s_env->chips[i] = next[i];
    }
    s_env->chip_n = w;
    s_env->cloud_suggest = false;
    fill_action_bar();
    return true;
}

void nodelio_skill_support_cloud_suggest_failed(void)
{
    DisplayLockGuard lock(nl_ov_disp());
    if (!lock || !s_have_env || !s_env || strcmp(s_env->card_type, "support_thread") != 0) {
        return;
    }
    s_env->cloud_suggest = false;
    for (int i = 0; i < s_env->chip_n; ++i) {
        if (strcmp(s_env->chips[i].id, "suggest_fail") != 0) {
            continue;
        }
        snprintf(s_env->chips[i].title, sizeof(s_env->chips[i].title), "知道了");
        cJSON *obj = cJSON_Parse(s_env->chips[i].params_json);
        if (!cJSON_IsObject(obj)) {
            cJSON_Delete(obj);
            obj = cJSON_CreateObject();
        }
        if (!obj) {
            break;
        }
        cJSON_DeleteItemFromObject(obj, "text");
        cJSON_AddStringToObject(obj, "text", "暂时无法根据这句话生成回复");
        char *printed = cJSON_PrintUnformatted(obj);
        cJSON_Delete(obj);
        if (printed) {
            snprintf(s_env->chips[i].params_json, sizeof(s_env->chips[i].params_json), "%s", printed);
            cJSON_free(printed);
        }
        break;
    }
    fill_action_bar();
}

bool nodelio_skill_support_voice_armed(void)
{
    return s_voice_listen;
}

void nodelio_skill_support_voice_fail(const char *reason)
{
    if (!s_voice_listen) {
        return;
    }
    s_voice_listen = false;
    DisplayLockGuard lock(nl_ov_disp());
    if (lock) {
        fill_action_bar();
    }
    nl_ov_toast(reason && reason[0] ? reason : "没连上小智");
}

bool nodelio_skill_try_support_voice_draft(const char *text)
{
    if (!text || !text[0] || !s_have_env || !s_env || !s_voice_listen) {
        return false;
    }
    if (strcmp(s_env->card_type, "support_thread") != 0) {
        return false;
    }
    if (s_env->app_id[0] && !strstr(s_env->app_id, "support.inbox")) {
        return false;
    }
    if (!nodelio_skill_result_card_visible()) {
        return false;
    }
    uint64_t cid = 0;
    if (!nodelio_skill_support_conversation_id(&cid) || cid == 0) {
        return false;
    }
    snprintf(s_voice_draft, sizeof(s_voice_draft), "%s", text);
    s_voice_draft_cid = cid;
    s_voice_listen = false;
    DisplayLockGuard lock(nl_ov_disp());
    if (lock) {
        nl_ov_dismiss_ai_chat();
        fill_action_bar();
    }
    nl_ov_toast("已记下，点发送发给访客");
    return true;
}
