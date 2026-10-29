/* 由 skill_result_ui.cc 在 fill_list 定义前包含。依赖该文件的静态状态。 */

static void fill_list(void);
static bool news_step_locked(int delta, char *title, size_t title_len);
static void news_fetch_article(const news_job_t *job);
bool nodelio_skill_news_action(const char *op, char *out, size_t out_len);
static bool news_load_spoken(int idx, uint32_t gen, char *out, size_t out_len, bool reset_chunk);
static void news_speak_clear(void);
static void news_apply_body_zoom(void);

static void news_abs_url(const char *in, char *out, size_t out_len)
{
    out[0] = '\0';
    if (!in || !in[0] || out_len == 0) {
        return;
    }
    if (strncmp(in, "http://", 7) == 0 || strncmp(in, "https://", 8) == 0) {
        snprintf(out, out_len, "%s", in);
        return;
    }
    if (in[0] == '/' && s_skill_cfg.server_url[0]) {
        char base[160];
        nl_url_strip_base(s_skill_cfg.server_url, base, sizeof(base));
        snprintf(out, out_len, "%s%s", base, in);
    }
}

static void news_on_screen(bool on)
{
    if (on) {
        return;
    }
    s_news_gen += 1;
}

static void news_worker(void *arg)
{
    (void)arg;
    news_job_t job;
    for (;;) {
        if (xQueueReceive(s_news_q, &job, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        if (job.nav != 0) {
            char spoken[32];
            const char *op = job.nav < 0 ? "prev" : "next";
            if (!nodelio_skill_news_action(op, spoken, sizeof(spoken))) {
                DisplayLockGuard lock(nl_ov_disp());
                if (lock) {
                    nl_ov_toast(job.nav < 0 ? "没有上一篇" : "没有下一篇");
                }
            }
            continue;
        }
        if (job.gen != s_news_gen || !nodelio_panel_ui_screen_on()) {
            continue;
        }
        news_fetch_article(&job);
    }
}

static void news_ensure_worker(void)
{
    if (s_news_q) {
        return;
    }
    s_news_q = xQueueCreate(4, sizeof(news_job_t));
    if (!s_news_q) {
        return;
    }
    if (!s_news_stack) {
        s_news_stack =
            static_cast<StackType_t *>(heap_caps_malloc(NEWS_TASK_STACK, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    }
    if (s_news_stack) {
        xTaskCreateStatic(news_worker, "nl_news", NEWS_TASK_STACK / sizeof(StackType_t), nullptr, 3, s_news_stack,
                          &s_news_tcb);
    } else {
        xTaskCreate(news_worker, "nl_news", NEWS_TASK_STACK, nullptr, 3, nullptr);
    }
}

static char *s_speak_body = nullptr;
static size_t s_speak_len = 0;
static size_t s_speak_off = 0;
/** 刚交给语音的这一段在正文里的字节起点，用来跟读滚屏 */
static size_t s_speak_off_prev = 0;
static char s_speak_id[NL_ENV_STR];
static char s_speak_title[NL_ENV_NEWS_TITLE];
static NewsReadProgress s_news_read_progress;
static uint32_t s_news_read_epoch = 0;
/** true=仅摘要/标题，勿让模型声称「全文已读完」 */
static bool s_speak_from_summary = false;
static void news_scroll_to_byte(size_t byte_off);

/** 单飞缓存：语音 read 与 UI worker 共用一次 HTTP，避免 net-lock 竞态把摘要盖上正文 */
static SemaphoreHandle_t s_pull_mu = nullptr;
static char s_cache_id[NL_ENV_STR];
static char *s_cache_body = nullptr;
static bool s_cache_from_summary = false;

static void *news_cjson_malloc(size_t sz)
{
    void *p = heap_caps_malloc(sz, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return p ? p : malloc(sz);
}

static void news_cjson_free(void *p)
{
    free(p);
}

static void news_ensure_cjson_psram(void)
{
    static bool once;
    if (once) {
        return;
    }
    once = true;
    cJSON_Hooks hooks = {
        .malloc_fn = news_cjson_malloc,
        .free_fn = news_cjson_free,
    };
    cJSON_InitHooks(&hooks);
}

static void news_cache_clear(void)
{
    if (s_cache_body) {
        heap_caps_free(s_cache_body);
        s_cache_body = nullptr;
    }
    s_cache_id[0] = '\0';
    s_cache_from_summary = false;
}

static void news_body_heap_clear(void)
{
    if (s_news_body_heap) {
        heap_caps_free(s_news_body_heap);
        s_news_body_heap = nullptr;
    }
}

static void news_speak_clear(void)
{
    DisplayLockGuard lock(nl_ov_disp());
    if (!lock) {
        return;
    }
    s_news_read_progress.Clear();
    ++s_news_read_epoch;
    if (s_speak_body) {
        heap_caps_free(s_speak_body);
        s_speak_body = nullptr;
    }
    s_speak_len = 0;
    s_speak_off = 0;
    s_speak_off_prev = 0;
    s_speak_id[0] = '\0';
    s_speak_title[0] = '\0';
    s_speak_from_summary = false;
}

static size_t news_utf8_floor(const char *s, size_t off)
{
    if (!s || off == 0) {
        return 0;
    }
    while (off > 0 && (static_cast<unsigned char>(s[off]) & 0xC0) == 0x80) {
        off--;
    }
    return off;
}

static size_t news_utf8_chunk_end(const char *s, size_t off, size_t max_bytes)
{
    if (!s || max_bytes == 0) {
        return off;
    }
    const size_t n = strlen(s);
    if (off >= n) {
        return n;
    }
    size_t end = off + max_bytes;
    if (end >= n) {
        return n;
    }
    while (end > off && (static_cast<unsigned char>(s[end]) & 0xC0) == 0x80) {
        end--;
    }
    if (end == off) {
        end = off + 1;
        while (end < n && (static_cast<unsigned char>(s[end]) & 0xC0) == 0x80) {
            end++;
        }
    }
    // Prefer a sentence boundary in the latter half of the budget. This avoids
    // cutting a sentence/word in two and inviting the model to invent its ending.
    size_t sentence_end = 0, clause_end = 0;
    for (size_t i = off + (end - off) / 2; i < end; ++i) {
        if (s[i] == '\n' || s[i] == '!' || s[i] == '?' ||
            (s[i] == '.' && (i + 1 == n || s[i + 1] == ' ' || s[i + 1] == '\n'))) {
            sentence_end = i + 1;
        } else if (i + 3 <= end) {
            if (memcmp(s + i, "。", 3) == 0 || memcmp(s + i, "！", 3) == 0 ||
                memcmp(s + i, "？", 3) == 0) {
                sentence_end = i + 3;
            } else if (memcmp(s + i, "，", 3) == 0 || memcmp(s + i, "；", 3) == 0) {
                clause_end = i + 3;
            }
        }
    }
    if (sentence_end) {
        return sentence_end;
    }
    if (clause_end) {
        return clause_end;
    }
    return end;
}

static void news_speak_set(const char *id, const char *title, const char *body, bool from_summary)
{
    DisplayLockGuard lock(nl_ov_disp());
    if (!lock) {
        return;
    }
    news_speak_clear();
    s_speak_from_summary = from_summary;
    if (id) {
        snprintf(s_speak_id, sizeof(s_speak_id), "%s", id);
    }
    if (title) {
        snprintf(s_speak_title, sizeof(s_speak_title), "%s", title);
    }
    if (!body || !body[0]) {
        return;
    }
    const size_t n = strlen(body);
    s_speak_body = static_cast<char *>(heap_caps_malloc(n + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!s_speak_body) {
        s_speak_body = static_cast<char *>(malloc(n + 1));
    }
    if (!s_speak_body) {
        return;
    }
    memcpy(s_speak_body, body, n + 1);
    s_speak_len = n;
    s_speak_off = 0;
    s_speak_off_prev = 0;
}

/** 播报到哪，详情滚到哪。可在新闻 worker / Schedule 回调里调（内部加 Display 锁）。 */
static void news_scroll_to_byte(size_t byte_off)
{
    DisplayLockGuard lock(nl_ov_disp());
    if (!lock || !s_card_list || !s_news_body_lbl || !lv_obj_is_valid(s_news_body_lbl) ||
        s_news_detail < 0 || s_news_detail >= s_env->news_n || !s_speak_body) {
        return;
    }
    if (strcmp(s_speak_id, s_env->news[s_news_detail].id) != 0) {
        return;
    }
    if (!lv_obj_has_flag(s_card_list, LV_OBJ_FLAG_SCROLLABLE)) {
        return;
    }
    const char *txt = lv_label_get_text(s_news_body_lbl);
    if (!txt || !txt[0] || strstr(txt, "正在加载") != nullptr) {
        return;
    }
    /* 摘要标签带「（仅摘要）\n」前缀，朗读正文从换行后算 */
    size_t map_off = byte_off;
    if (s_speak_from_summary) {
        const char *nl = strchr(txt, '\n');
        if (nl) {
            map_off += static_cast<size_t>(nl - txt) + 1;
        }
    }
    if (map_off > strlen(txt)) {
        map_off = strlen(txt);
    }
    map_off = news_utf8_floor(txt, map_off);

    lv_obj_update_layout(s_card_list);
    lv_obj_update_layout(s_news_body_lbl);

    uint32_t char_id = 0;
    size_t i = 0;
    while (txt[i] && i < map_off) {
        if ((static_cast<unsigned char>(txt[i]) & 0xC0) != 0x80) {
            char_id++;
        }
        i++;
    }
    lv_point_t pos = {};
    lv_label_get_letter_pos(s_news_body_lbl, char_id, &pos);
    const lv_coord_t view_h = lv_obj_get_content_height(s_card_list);
    lv_coord_t y = lv_obj_get_y(s_news_body_lbl) + pos.y - (view_h > 0 ? view_h / 4 : 40);
    if (y < 0) {
        y = 0;
    }
    const lv_coord_t max_y = lv_obj_get_scroll_bottom(s_card_list) + lv_obj_get_scroll_y(s_card_list);
    if (max_y > 0 && y > max_y) {
        y = max_y;
    }
    lv_obj_scroll_to_y(s_card_list, y, LV_ANIM_ON);
}

static bool news_read_is_current(void) {
    return s_have_env && s_env && s_speak_body && s_news_detail >= 0 &&
           s_news_detail < s_env->news_n && skill_ui_is_up() &&
           strcmp(s_env->card_type, "news_list") == 0 &&
           strcmp(s_speak_id, s_env->news[s_news_detail].id) == 0;
}

uint32_t nodelio_skill_news_read_token(void) {
    DisplayLockGuard lock(nl_ov_disp());
    return lock && news_read_is_current() ? s_news_read_progress.token() : 0;
}

uint32_t nodelio_skill_news_on_sentence(const char* text) {
    DisplayLockGuard lock(nl_ov_disp());
    size_t byte_offset = 0;
    if (!lock || !text || !news_read_is_current() ||
        !s_news_read_progress.Sentence(text, byte_offset)) {
        return 0;
    }
    if (nodelio_panel_ui_screen_on()) {
        news_scroll_to_byte(byte_offset);
    }
    return s_news_read_progress.token();
}

bool nodelio_skill_news_finish_chunk(uint32_t token, bool played_audio, bool* more) {
    DisplayLockGuard lock(nl_ov_disp());
    bool next = false;
    if (!lock || !news_read_is_current() ||
        !s_news_read_progress.Finish(token, played_audio, next)) {
        return false;
    }
    if (more) {
        *more = next;
    }
    return true;
}

void nodelio_skill_news_cancel_read(void) {
    DisplayLockGuard lock(nl_ov_disp());
    if (lock) {
        s_news_read_progress.Cancel();
        ++s_news_read_epoch;
    }
}

/** 切下一段原文到 piece；实际句子事件到达后才滚屏。piece 由调用方 free。 */
static char *news_take_piece(bool *more_out, size_t max_bytes)
{
    DisplayLockGuard lock(nl_ov_disp());
    if (!lock) {
        return nullptr;
    }
    if (more_out) {
        *more_out = false;
    }
    if (!s_speak_body || s_speak_off >= s_speak_len) {
        return nullptr;
    }
    if (max_bytes < 48) {
        max_bytes = 48;
    }
    const size_t end = news_utf8_chunk_end(s_speak_body, s_speak_off, max_bytes);
    const size_t take = end > s_speak_off ? end - s_speak_off : 0;
    if (more_out) {
        *more_out = end < s_speak_len;
    }
    char *piece = static_cast<char *>(heap_caps_malloc(take + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!piece) {
        piece = static_cast<char *>(malloc(take + 1));
    }
    if (!piece) {
        return nullptr;
    }
    memcpy(piece, s_speak_body + s_speak_off, take);
    piece[take] = '\0';
    s_speak_off_prev = s_speak_off;
    s_speak_off = end;
    s_news_read_progress.Begin(std::string_view(s_speak_body + s_speak_off_prev, take),
                               s_speak_off_prev, end < s_speak_len);
    return piece;
}

static bool news_format_plain_chunk(char *out, size_t out_len, bool with_title)
{
    if (!out || out_len == 0) {
        return false;
    }
    out[0] = '\0';
    /* 标题占额度；正文用 LOCAL 上限，整包 detect JSON 须 ≤720B */
    size_t body_budget = NEWS_LOCAL_SPEAK_CHUNK;
    if (with_title && s_speak_title[0]) {
        const size_t tlen = strlen(s_speak_title);
        if (tlen + 8 < body_budget) {
            body_budget -= tlen + 8;
        } else {
            body_budget = 80;
        }
    }
    bool more = false;
    char *piece = news_take_piece(&more, body_budget);
    if (!piece) {
        if (with_title && s_speak_title[0]) {
            snprintf(out, out_len, "%s。正文暂不可用。", s_speak_title);
            return true;
        }
        return false;
    }
    /* 本地 detect 接力续段，勿把「可说继续读」念出声 */
    (void)more;
    if (with_title && s_speak_title[0]) {
        snprintf(out, out_len, "%s。%s", s_speak_title, piece);
    } else {
        snprintf(out, out_len, "%s", piece);
    }
    heap_caps_free(piece);
    return out[0] != '\0';
}

static bool news_format_chunk(char *out, size_t out_len, bool with_title)
{
    DisplayLockGuard lock(nl_ov_disp());
    if (!lock) {
        return false;
    }
    if (!out || out_len == 0) {
        return false;
    }
    out[0] = '\0';
    if (!s_speak_body || s_speak_off >= s_speak_len) {
        if (with_title && s_speak_title[0]) {
            snprintf(out, out_len,
                     "%s\n（没有可朗读的正文，请稍后重试 op=read，不要告诉用户已经读完）", s_speak_title);
            return true;
        }
        return false;
    }
    // Budget the JSON-escaped text, not just UTF-8 bytes. Quotes, newlines and
    // control characters otherwise make a small article exceed the MQTT limit.
    size_t raw_bytes = 0;
    size_t json_bytes = 0;
    const char* remaining = s_speak_body + s_speak_off;
    while (remaining[raw_bytes]) {
        const unsigned char c = static_cast<unsigned char>(remaining[raw_bytes]);
        const size_t cost = c < 0x20 ? 6 : (c == '"' || c == '\\' ? 2 : 1);
        if (json_bytes + cost > NEWS_MCP_CHUNK) {
            break;
        }
        json_bytes += cost;
        raw_bytes++;
    }
    bool more = false;
    char* piece = news_take_piece(&more, raw_bytes);
    if (!piece) {
        return false;
    }
    const char* tail_more =
        "\n【续】请先逐字朗读以上原文，不要摘要、不要说读完。设备会在播完后请求下一段；本轮不要自行"
        "调用 continue，不要读出这些指令。";
    const char *tail_done =
        s_speak_from_summary
            ? "\n【摘要】以上仅为摘要，请逐字朗读；不要声称全文已读完。"
            : "\n【完】请逐字朗读以上原文；念完后可告知本段结束，不要在朗读前说已经读完。";
    if (with_title && s_speak_title[0]) {
        snprintf(out, out_len, "逐字朗读下面原文，不要摘要，不要换篇：\n%s\n%s%s", s_speak_title, piece,
                 more ? tail_more : tail_done);
    } else {
        snprintf(out, out_len, "逐字朗读下面原文，不要摘要：\n%s%s", piece, more ? tail_more : tail_done);
    }
    heap_caps_free(piece);
    return out[0] != '\0';
}

static char *news_dup_psram(const char *s)
{
    if (!s || !s[0]) {
        return nullptr;
    }
    const size_t n = strlen(s);
    char *copy = static_cast<char *>(heap_caps_malloc(n + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!copy) {
        copy = static_cast<char *>(malloc(n + 1));
    }
    if (!copy) {
        return nullptr;
    }
    memcpy(copy, s, n + 1);
    return copy;
}

/**
 * 拉单篇正文（单飞：同 id 并发只打一次 HTTP）。
 * @param out_heap 成功时为可 free 的正文拷贝（PSRAM）；失败为 nullptr 且可用 *out_view 看回退串
 * @param out_view 始终指向可用展示文本（heap 或 summary/title 静态区）
 * @param from_summary 未拿到 article body、仅用摘要/标题
 */
static bool news_pull_article_text(int idx, char **out_heap, const char **out_view, bool *from_summary)
{
    if (out_heap) {
        *out_heap = nullptr;
    }
    if (out_view) {
        *out_view = nullptr;
    }
    if (from_summary) {
        *from_summary = true;
    }
    if (idx < 0 || idx >= s_env->news_n || !s_env->news[idx].id[0]) {
        return false;
    }
    const char *id = s_env->news[idx].id;
    const char *fallback =
        s_env->news[idx].summary[0] ? s_env->news[idx].summary : s_env->news[idx].title;
    if (out_view) {
        *out_view = fallback;
    }

    if (!s_pull_mu) {
        s_pull_mu = xSemaphoreCreateMutex();
    }
    if (s_pull_mu) {
        xSemaphoreTake(s_pull_mu, portMAX_DELAY);
    }

    bool ok = false;
    if (s_cache_body && strcmp(s_cache_id, id) == 0) {
        char *copy = news_dup_psram(s_cache_body);
        if (copy) {
            if (out_heap) {
                *out_heap = copy;
            }
            if (out_view) {
                *out_view = copy;
            }
            if (from_summary) {
                *from_summary = s_cache_from_summary;
            }
            ok = true;
        } else {
            if (out_view) {
                *out_view = s_cache_body;
            }
            if (from_summary) {
                *from_summary = s_cache_from_summary;
            }
            ok = true;
        }
        if (s_pull_mu) {
            xSemaphoreGive(s_pull_mu);
        }
        return ok;
    }

    char abs_url[512];
    abs_url[0] = '\0';
    if (s_skill_cfg.device_id[0]) {
        char path[320];
        snprintf(path, sizeof(path), "/api/devices/%s/panel/news/article?id=%s", s_skill_cfg.device_id, id);
        news_abs_url(path, abs_url, sizeof(abs_url));
    }
    if (!abs_url[0]) {
        if (s_pull_mu) {
            xSemaphoreGive(s_pull_mu);
        }
        return fallback && fallback[0];
    }

    char *buf = static_cast<char *>(heap_caps_malloc(NEWS_ARTICLE_CAP, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!buf) {
        buf = static_cast<char *>(malloc(NEWS_ARTICLE_CAP));
    }
    if (!buf) {
        if (s_pull_mu) {
            xSemaphoreGive(s_pull_mu);
        }
        return fallback && fallback[0];
    }

    news_ensure_cjson_psram();
    int status = 0;
    esp_err_t err = nodelio_http_get_ms(abs_url, s_skill_cfg.device_key, nullptr, &status, buf,
                                        NEWS_ARTICLE_CAP - 1, NEWS_HTTP_IO_MS);
    if (err == ESP_OK && status == 200) {
        cJSON *root = cJSON_Parse(buf);
        cJSON *body = root ? cJSON_GetObjectItem(root, "body") : nullptr;
        if (cJSON_IsString(body) && body->valuestring && body->valuestring[0]) {
            char *copy = news_dup_psram(body->valuestring);
            if (copy) {
                const cJSON *is_sum = cJSON_GetObjectItem(root, "is_summary");
                const bool only_sum =
                    cJSON_IsTrue(is_sum) || (fallback && strcmp(copy, fallback) == 0);
                news_cache_clear();
                snprintf(s_cache_id, sizeof(s_cache_id), "%s", id);
                s_cache_body = news_dup_psram(copy);
                s_cache_from_summary = only_sum;
                if (out_heap) {
                    *out_heap = copy;
                } else {
                    heap_caps_free(copy);
                    copy = nullptr;
                }
                if (out_view) {
                    *out_view = out_heap && *out_heap ? *out_heap : (s_cache_body ? s_cache_body : fallback);
                }
                if (from_summary) {
                    *from_summary = only_sum;
                }
                ok = true;
            }
        } else {
            ESP_LOGW("nl_news", "article json no body id=%s status=%d", id, status);
        }
        if (root) {
            cJSON_Delete(root);
        }
    } else {
        ESP_LOGW("nl_news", "article http id=%s err=%d status=%d", id, (int)err, status);
    }
    heap_caps_free(buf);

    if (!ok) {
        ok = fallback && fallback[0];
        if (ok && from_summary) {
            *from_summary = true;
        }
    }
    if (s_pull_mu) {
        xSemaphoreGive(s_pull_mu);
    }
    return ok;
}

/** 详情字号档：0=14px，1/2=20px 真字库（会话内记住）。默认较大，墙板远读。 */
static uint8_t s_news_zoom = 1;
static lv_obj_t *s_news_zoom_hint = nullptr;
static lv_obj_t *s_news_title_lbl = nullptr;

static void on_news_zoom(lv_event_t *e);
static const char *news_zoom_name(void);

static lv_obj_t *news_make_zoom_chip(lv_obj_t *parent, const char *txt, intptr_t delta)
{
    lv_obj_t *b = lv_obj_create(parent);
    lv_obj_remove_style_all(b);
    lv_obj_set_size(b, 40, 40);
    lv_obj_set_style_radius(b, 12, 0);
    lv_obj_set_style_bg_color(b, UI_CAPSULE_OFF, 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(b, UI_SURFACE_PRESSED, LV_STATE_PRESSED);
    lv_obj_set_style_translate_y(b, 1, LV_STATE_PRESSED);
    lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(b, UI_EXT_CLICK);
    lv_obj_add_event_cb(b, on_news_zoom, LV_EVENT_CLICKED, (void *)delta);
    lv_obj_t *l = lv_label_create(b);
    nl_ov_set_font(l);
    lv_label_set_text(l, txt);
    lv_obj_set_style_text_color(l, UI_ACCENT_SOFT, 0);
    lv_obj_center(l);
    return b;
}

static void ensure_news_zoom_hdr(void)
{
    if (s_news_zoom_bar || !s_hdr_btns) {
        return;
    }
    s_news_zoom_bar = lv_obj_create(s_hdr_btns);
    lv_obj_remove_style_all(s_news_zoom_bar);
    lv_obj_set_height(s_news_zoom_bar, 44);
    lv_obj_set_width(s_news_zoom_bar, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(s_news_zoom_bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(s_news_zoom_bar, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(s_news_zoom_bar, 6, 0);
    lv_obj_clear_flag(s_news_zoom_bar, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_move_to_index(s_news_zoom_bar, 0); /* 刷新按钮左侧 */

    news_make_zoom_chip(s_news_zoom_bar, "A-", (intptr_t)-1);

    s_news_zoom_hint = lv_label_create(s_news_zoom_bar);
    nl_ov_set_font(s_news_zoom_hint);
    lv_label_set_text(s_news_zoom_hint, news_zoom_name());
    lv_obj_set_style_text_color(s_news_zoom_hint, UI_TEXT_MUTED, 0);

    news_make_zoom_chip(s_news_zoom_bar, "A+", (intptr_t)1);

    lv_obj_add_flag(s_news_zoom_bar, LV_OBJ_FLAG_HIDDEN);
}

static void set_news_zoom_hdr_visible(bool on)
{
    ensure_news_zoom_hdr();
    if (!s_news_zoom_bar || !lv_obj_is_valid(s_news_zoom_bar)) {
        return;
    }
    if (on) {
        if (s_news_zoom_hint && lv_obj_is_valid(s_news_zoom_hint)) {
            lv_label_set_text(s_news_zoom_hint, news_zoom_name());
        }
        lv_obj_clear_flag(s_news_zoom_bar, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_news_zoom_bar, LV_OBJ_FLAG_HIDDEN);
    }
}

static const char *news_zoom_name(void)
{
    static const char *k_name[] = {"标准", "较大", "疏朗"};
    if (s_news_zoom > UI_NEWS_ZOOM_MAX) {
        return k_name[0];
    }
    return k_name[s_news_zoom];
}

static const lv_font_t *news_body_font(void)
{
    return s_news_zoom == 0 ? nl_ov_ui_font() : nl_ov_read_font();
}

static int32_t news_body_line_space(void)
{
    static const int32_t k_ls[] = {UI_NEWS_LINE_SPACE_0, UI_NEWS_LINE_SPACE_1, UI_NEWS_LINE_SPACE_2};
    if (s_news_zoom > UI_NEWS_ZOOM_MAX) {
        return k_ls[0];
    }
    return k_ls[s_news_zoom];
}

/** 真字库换档 + 行距；不用 transform，避免 14px 点阵被拉糊。 */
static void news_apply_body_zoom(void)
{
    const lv_font_t *font = news_body_font();
    const int32_t ls = news_body_line_space();
    const int32_t letter = (s_news_zoom >= 2) ? UI_NEWS_LETTER_SPACE_2 : 0;
    if (s_news_body_lbl && lv_obj_is_valid(s_news_body_lbl)) {
        lv_obj_set_style_text_font(s_news_body_lbl, font, 0);
        lv_obj_set_style_text_line_space(s_news_body_lbl, ls, 0);
        lv_obj_set_style_text_letter_space(s_news_body_lbl, letter, 0);
        lv_obj_set_width(s_news_body_lbl, lv_pct(100));
        lv_obj_set_height(s_news_body_lbl, LV_SIZE_CONTENT);
        lv_obj_set_style_transform_scale_x(s_news_body_lbl, LV_SCALE_NONE, 0);
        lv_obj_set_style_transform_scale_y(s_news_body_lbl, LV_SCALE_NONE, 0);
        lv_obj_set_style_pad_bottom(s_news_body_lbl, 16, 0);
    }
    if (s_news_title_lbl && lv_obj_is_valid(s_news_title_lbl)) {
        lv_obj_set_style_text_font(s_news_title_lbl, font, 0);
        lv_obj_set_style_text_line_space(s_news_title_lbl, ls > 4 ? 4 : 2, 0);
        lv_obj_set_width(s_news_title_lbl, lv_pct(100));
    }
    if (s_news_zoom_hint && lv_obj_is_valid(s_news_zoom_hint)) {
        lv_label_set_text(s_news_zoom_hint, news_zoom_name());
        lv_obj_set_style_text_color(s_news_zoom_hint, UI_TEXT_MUTED, 0);
    }
}

static void news_apply_body_label(int idx, uint32_t gen, const char *text, bool from_summary)
{
    if (!text || gen != s_news_gen || !nodelio_panel_ui_screen_on()) {
        return;
    }
    char *owned = nullptr;
    if (from_summary) {
        const size_t n = strlen(text) + 32;
        owned = static_cast<char *>(heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        if (!owned) {
            owned = static_cast<char *>(malloc(n));
        }
        if (owned) {
            snprintf(owned, n, "（仅摘要）\n%s", text);
        }
    } else {
        owned = news_dup_psram(text);
    }
    if (!owned) {
        return;
    }
    DisplayLockGuard lock(nl_ov_disp());
    if (!lock || gen != s_news_gen || s_news_detail != idx || !s_news_body_lbl ||
        !lv_obj_is_valid(s_news_body_lbl)) {
        heap_caps_free(owned);
        return;
    }
    news_body_heap_clear();
    s_news_body_heap = owned;
    lv_label_set_text_static(s_news_body_lbl, s_news_body_heap);
    lv_obj_set_style_text_color(s_news_body_lbl, from_summary ? UI_TEXT_DIM : UI_TEXT, 0);
    news_apply_body_zoom();
}

/** 语音朗读：拉正文进缓存并吐出一段。UI 预加载勿走此路径（会打乱 off）。 */
static bool news_load_spoken(int idx, uint32_t gen, char *out, size_t out_len, bool reset_chunk)
{
    if (out && out_len > 0) {
        out[0] = '\0';
    }
    char title[NL_ENV_NEWS_TITLE], id[NL_ENV_STR];
    uint32_t read_epoch = 0;
    {
        DisplayLockGuard lock(nl_ov_disp());
        if (!lock || !s_env || gen != s_news_gen || idx != s_news_detail || idx < 0 ||
            idx >= s_env->news_n || !s_env->news[idx].id[0]) {
            return false;
        }
        snprintf(title, sizeof(title), "%s", s_env->news[idx].title);
        snprintf(id, sizeof(id), "%s", s_env->news[idx].id);
        read_epoch = s_news_read_epoch;
        if (!reset_chunk && s_speak_body && strcmp(s_speak_id, id) == 0) {
            return news_format_chunk(out, out_len, false);
        }
    }
    char *heap = nullptr;
    const char *view = nullptr;
    bool from_summary = true;
    if (!news_pull_article_text(idx, &heap, &view, &from_summary) || !view || !view[0]) {
        if (out && out_len > 0) {
            snprintf(out, out_len, "%s\n（正文暂不可用，请稍后重试，不要说已经读完）",
                     title[0] ? title : "这篇新闻");
        }
        return out && out[0];
    }
    DisplayLockGuard lock(nl_ov_disp());
    if (!lock || !s_env || gen != s_news_gen || read_epoch != s_news_read_epoch ||
        idx != s_news_detail || idx >= s_env->news_n || strcmp(id, s_env->news[idx].id) != 0) {
        heap_caps_free(heap);
        return false;
    }
    news_apply_body_label(idx, gen, view, from_summary);
    news_speak_set(id, title, view, from_summary);
    if (heap) {
        heap_caps_free(heap);
    }
    if (!out || out_len == 0) {
        return true;
    }
    return news_format_chunk(out, out_len, true);
}

/** 仅刷新详情页正文，不碰朗读游标（避免与语音 read 竞态）。 */
static void news_fetch_article(const news_job_t *job)
{
    if (!job) {
        return;
    }
    char *heap = nullptr;
    const char *view = nullptr;
    bool from_summary = true;
    if (!news_pull_article_text(job->slot, &heap, &view, &from_summary) || !view) {
        if (heap) {
            heap_caps_free(heap);
        }
        return;
    }
    news_apply_body_label(job->slot, job->gen, view, from_summary);
    if (heap) {
        heap_caps_free(heap);
    }
}

static bool news_post_mark_read(const char *item_id)
{
    if (!item_id || !item_id[0] || !s_skill_cfg.device_id[0] || !s_skill_cfg.server_url[0]) {
        return false;
    }
    char abs_url[512];
    char path[320];
    snprintf(path, sizeof(path), "/api/devices/%s/panel/news/read", s_skill_cfg.device_id);
    news_abs_url(path, abs_url, sizeof(abs_url));
    if (!abs_url[0]) {
        return false;
    }
    char body[96];
    snprintf(body, sizeof(body), "{\"id\":\"%s\"}", item_id);
    char resp[128];
    int status = 0;
    esp_err_t err = nodelio_http_post_json(abs_url, s_skill_cfg.device_key, body, &status, resp, sizeof(resp) - 1);
    if (err != ESP_OK || status < 200 || status >= 300) {
        ESP_LOGW("nl_news", "mark read id=%s status=%d err=%d", item_id, status, (int)err);
        return false;
    }
    ESP_LOGI("nl_news", "mark read id=%s", item_id);
    return true;
}

static void news_remove_local(int idx)
{
    if (idx < 0 || idx >= s_env->news_n) {
        return;
    }
    for (int i = idx; i + 1 < s_env->news_n; i++) {
        s_env->news[i] = s_env->news[i + 1];
    }
    s_env->news_n -= 1;
    if (s_env->news_n <= 0) {
        s_news_detail = -1;
    } else if (s_news_detail >= s_env->news_n) {
        s_news_detail = s_env->news_n - 1;
    } else if (s_news_detail > idx) {
        s_news_detail -= 1;
    }
}

static void news_enqueue_article(int idx)
{
    if (idx < 0 || idx >= s_env->news_n || !s_env->news[idx].id[0]) {
        return;
    }
    if (!nodelio_panel_ui_screen_on() || !s_skill_cfg.device_id[0]) {
        return;
    }
    news_ensure_worker();
    if (!s_news_q) {
        return;
    }
    news_job_t job;
    memset(&job, 0, sizeof(job));
    job.slot = idx;
    job.gen = s_news_gen;
    snprintf(job.url, sizeof(job.url), "/api/devices/%s/panel/news/article?id=%s", s_skill_cfg.device_id,
             s_env->news[idx].id);
    xQueueSendToFront(s_news_q, &job, 0);
}

static void news_time_short(const char *iso, char *out, size_t out_len)
{
    out[0] = '\0';
    if (!iso || !iso[0] || out_len < 12) {
        return;
    }
    if (strlen(iso) >= 16 && iso[4] == '-' && iso[10] == 'T') {
        snprintf(out, out_len, "%.5s %.5s", iso + 5, iso + 11);
        return;
    }
    snprintf(out, out_len, "%s", iso);
}

static void on_news_row(lv_event_t *e)
{
    const int idx = (int)(intptr_t)lv_event_get_user_data(e);
    if (idx < 0 || idx >= s_env->news_n) {
        return;
    }
    s_news_detail = idx;
    fill_list();
}

static void on_news_back(lv_event_t *e)
{
    (void)e;
    s_news_detail = -1;
    fill_list();
}

static void on_news_read_btn(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) {
        return;
    }
    Application::GetInstance().Schedule([]() {
        auto &app = Application::GetInstance();
        const auto st = app.GetDeviceState();
        if (st != kDeviceStateListening && st != kDeviceStateSpeaking) {
            nl_ov_toast("请唤醒后说「读这篇」");
            return;
        }
        app.StartLocalNewsRead("read");
    });
}

static void on_news_nav(lv_event_t *e)
{
    const int delta = (int)(intptr_t)lv_event_get_user_data(e);
    if (delta == 0 || s_news_detail < 0) {
        nl_ov_toast(delta < 0 ? "没有上一篇" : "没有下一篇");
        return;
    }
    news_ensure_worker();
    if (!s_news_q) {
        nl_ov_toast("请稍候");
        return;
    }
    news_job_t job;
    memset(&job, 0, sizeof(job));
    job.nav = delta;
    job.gen = s_news_gen;
    if (xQueueSend(s_news_q, &job, 0) != pdTRUE) {
        nl_ov_toast("请稍候");
    }
}

static void on_news_zoom(lv_event_t *e)
{
    const int delta = (int)(intptr_t)lv_event_get_user_data(e);
    int next = (int)s_news_zoom + delta;
    if (next < 0) {
        next = 0;
        nl_ov_toast("已是最小字号");
        return;
    }
    if (next > UI_NEWS_ZOOM_MAX) {
        next = UI_NEWS_ZOOM_MAX;
        nl_ov_toast("已是最大字号");
        return;
    }
    if ((uint8_t)next == s_news_zoom) {
        return;
    }
    s_news_zoom = (uint8_t)next;
    news_apply_body_zoom();
}

static lv_obj_t *news_make_bar_btn(lv_obj_t *parent, const char *title, lv_event_cb_t cb, void *ud,
                                  bool primary)
{
    lv_obj_t *btn = make_text_action_btn(parent, title, cb, ud);
    lv_obj_set_height(btn, 44);
    lv_obj_set_width(btn, 0);
    lv_obj_set_flex_grow(btn, 1);
    lv_obj_set_style_radius(btn, 12, 0);
    lv_obj_set_style_translate_y(btn, 1, LV_STATE_PRESSED);
    if (!primary) {
        lv_obj_set_style_bg_color(btn, UI_CAPSULE_OFF, 0);
        lv_obj_t *lbl = lv_obj_get_child(btn, 0);
        if (lbl) {
            lv_obj_set_style_text_color(lbl, UI_TEXT_SUB, 0);
        }
    }
    return btn;
}

static void news_set_nav_bar(bool on)
{
    if (!s_dsh_detail_bar) {
        return;
    }
    lv_obj_clean(s_dsh_detail_bar);
    if (!on) {
        lv_obj_add_flag(s_dsh_detail_bar, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_height(s_dsh_detail_bar, 0);
        return;
    }
    lv_obj_set_flex_align(s_dsh_detail_bar, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    news_make_bar_btn(s_dsh_detail_bar, "上一篇", on_news_nav, (void *)(intptr_t)-1, false);
    news_make_bar_btn(s_dsh_detail_bar, "朗读", on_news_read_btn, nullptr, true);
    news_make_bar_btn(s_dsh_detail_bar, "下一篇", on_news_nav, (void *)(intptr_t)1, false);
    lv_obj_clear_flag(s_dsh_detail_bar, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_height(s_dsh_detail_bar, 52);
}

static void fill_news(void)
{
    set_hdr_mid_icon(MATERIAL_SYMBOLS_KEYBOARD_ARROW_DOWN);
    s_news_gen += 1;
    news_speak_clear();
    /* 不在此清 article 缓存：fill 可能与 HTTP 单飞并发；错 id 由 pull 内比对替换 */
    news_body_heap_clear();
    s_news_body_lbl = nullptr;
    s_news_title_lbl = nullptr;
    lv_obj_clean(s_card_list);
    lv_obj_add_flag(s_card_list, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(s_card_list, LV_DIR_VER);
    lv_obj_scroll_to_y(s_card_list, 0, LV_ANIM_OFF);

    if (strcmp(s_env->status, "ok") != 0 || s_env->news_n <= 0) {
        news_set_nav_bar(false);
        set_news_zoom_hdr_visible(false);
        if (s_card_sub) {
            lv_label_set_text(s_card_sub, "点关闭或点遮罩收起");
        }
        lv_obj_t *err = lv_label_create(s_card_list);
        nl_ov_set_font(err);
        const char *msg = s_env->error_message[0] ? s_env->error_message : (s_env->spoken[0] ? s_env->spoken : "无新闻");
        lv_label_set_text(err, msg);
        lv_obj_set_width(err, lv_pct(100));
        lv_label_set_long_mode(err, LV_LABEL_LONG_WRAP);
        lv_obj_set_style_text_color(err, strcmp(s_env->status, "ok") == 0 ? UI_TEXT_DIM : UI_DANGER_FG, 0);
        return;
    }

    if (s_news_detail >= s_env->news_n) {
        s_news_detail = -1;
    }

    if (s_news_detail >= 0) {
        const nl_env_news_t *n = &s_env->news[s_news_detail];
        set_news_zoom_hdr_visible(true);
        news_set_nav_bar(true);
        if (s_card_sub) {
            char sub[32];
            snprintf(sub, sizeof(sub), "%d / %d", s_news_detail + 1, s_env->news_n);
            lv_label_set_text(s_card_sub, sub);
        }
        lv_obj_set_style_pad_row(s_card_list, 10, 0);
        lv_obj_set_style_pad_bottom(s_card_list, 8, 0);

        lv_obj_t *back = make_text_action_btn(s_card_list, "返回列表", on_news_back, nullptr);
        lv_obj_set_size(back, LV_SIZE_CONTENT, 40);
        lv_obj_set_style_pad_hor(back, 14, 0);
        lv_obj_set_style_bg_color(back, UI_CAPSULE_OFF, 0);
        lv_obj_set_style_radius(back, 12, 0);
        {
            lv_obj_t *bl = lv_obj_get_child(back, 0);
            if (bl) {
                lv_obj_set_style_text_color(bl, UI_TEXT_SUB, 0);
            }
        }

        s_news_title_lbl = lv_label_create(s_card_list);
        lv_label_set_text(s_news_title_lbl, n->title);
        lv_obj_set_width(s_news_title_lbl, lv_pct(100));
        lv_label_set_long_mode(s_news_title_lbl, LV_LABEL_LONG_WRAP);
        lv_obj_set_style_text_color(s_news_title_lbl, UI_TEXT, 0);
        lv_obj_set_style_pad_top(s_news_title_lbl, 4, 0);

        char when[24];
        news_time_short(n->published_at, when, sizeof(when));
        char meta[96];
        if (n->source[0] && when[0]) {
            snprintf(meta, sizeof(meta), "%s  ·  %s", n->source, when);
        } else if (n->source[0]) {
            snprintf(meta, sizeof(meta), "%s", n->source);
        } else {
            snprintf(meta, sizeof(meta), "%s", when[0] ? when : " ");
        }
        lv_obj_t *meta_l = lv_label_create(s_card_list);
        nl_ov_set_font(meta_l);
        lv_label_set_text(meta_l, meta);
        lv_obj_set_width(meta_l, lv_pct(100));
        lv_label_set_long_mode(meta_l, LV_LABEL_LONG_DOT);
        lv_obj_set_style_text_color(meta_l, UI_TEXT_MUTED, 0);

        lv_obj_t *rule = lv_obj_create(s_card_list);
        lv_obj_remove_style_all(rule);
        lv_obj_set_width(rule, lv_pct(100));
        lv_obj_set_height(rule, 1);
        lv_obj_set_style_bg_color(rule, UI_BORDER, 0);
        lv_obj_set_style_bg_opa(rule, LV_OPA_COVER, 0);
        lv_obj_clear_flag(rule, LV_OBJ_FLAG_CLICKABLE);

        s_news_body_lbl = lv_label_create(s_card_list);
        /* 勿先贴 RSS 摘要（带「…」），否则长文加载失败时用户会以为详情只有概述 */
        lv_label_set_text(s_news_body_lbl, "正在加载详情…");
        lv_label_set_long_mode(s_news_body_lbl, LV_LABEL_LONG_WRAP);
        lv_obj_set_style_text_color(s_news_body_lbl, UI_TEXT_DIM, 0);
        lv_obj_set_style_pad_top(s_news_body_lbl, 2, 0);
        news_apply_body_zoom();
        news_enqueue_article(s_news_detail);
        return;
    }

    news_set_nav_bar(false);
    set_news_zoom_hdr_visible(false);
    if (s_card_sub) {
        char sub[48];
        snprintf(sub, sizeof(sub), "%d 条 · 点开看详情", s_env->news_n);
        lv_label_set_text(s_card_sub, sub);
    }

    lv_obj_set_style_pad_row(s_card_list, 10, 0);

    for (int i = 0; i < s_env->news_n; i++) {
        const nl_env_news_t *n = &s_env->news[i];
        lv_obj_t *row = lv_obj_create(s_card_list);
        lv_obj_remove_style_all(row);
        lv_obj_set_width(row, lv_pct(100));
        lv_obj_set_height(row, 82);
        lv_obj_set_style_bg_color(row, UI_FIELD, 0);
        lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(row, 14, 0);
        lv_obj_set_style_border_width(row, 1, 0);
        lv_obj_set_style_border_color(row, UI_TILE_OFF_BORDER, 0);
        lv_obj_set_style_border_opa(row, LV_OPA_60, 0);
        lv_obj_set_style_pad_hor(row, 12, 0);
        lv_obj_set_style_pad_ver(row, 8, 0);
        lv_obj_set_style_pad_column(row, 12, 0);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(row, on_news_row, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        lv_obj_set_style_bg_color(row, UI_SURFACE_PRESSED, LV_STATE_PRESSED);

        lv_obj_t *idx_pod = lv_obj_create(row);
        lv_obj_remove_style_all(idx_pod);
        lv_obj_set_size(idx_pod, 28, 28);
        lv_obj_set_style_radius(idx_pod, 8, 0);
        lv_obj_set_style_bg_color(idx_pod, UI_CAPSULE_OFF, 0);
        lv_obj_set_style_bg_opa(idx_pod, LV_OPA_COVER, 0);
        lv_obj_clear_flag(idx_pod, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_t *idx_l = lv_label_create(idx_pod);
        nl_ov_set_font(idx_l);
        char idx_buf[8];
        snprintf(idx_buf, sizeof(idx_buf), "%d", i + 1);
        lv_label_set_text(idx_l, idx_buf);
        lv_obj_set_style_text_color(idx_l, UI_ACCENT_SOFT, 0);
        lv_obj_center(idx_l);

        lv_obj_t *txt = lv_obj_create(row);
        lv_obj_remove_style_all(txt);
        lv_obj_set_flex_grow(txt, 1);
        lv_obj_set_height(txt, LV_SIZE_CONTENT);
        lv_obj_set_style_min_width(txt, 0, 0);
        lv_obj_set_flex_flow(txt, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_pad_row(txt, 5, 0);
        lv_obj_clear_flag(txt, LV_OBJ_FLAG_CLICKABLE);

        lv_obj_t *t = lv_label_create(txt);
        nl_ov_set_font(t);
        lv_label_set_text(t, n->title);
        lv_obj_set_style_text_color(t, UI_TEXT, 0);
        lv_label_set_long_mode(t, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(t, lv_pct(100));
        {
            const lv_font_t *font = lv_obj_get_style_text_font(t, LV_PART_MAIN);
            const int lh = font ? (int)lv_font_get_line_height(font) : 16;
            lv_obj_set_height(t, lh * 2);
        }

        char when[24];
        news_time_short(n->published_at, when, sizeof(when));
        char meta[96];
        if (n->source[0] && when[0]) {
            snprintf(meta, sizeof(meta), "%s · %s", n->source, when);
        } else if (n->source[0]) {
            snprintf(meta, sizeof(meta), "%s", n->source);
        } else {
            snprintf(meta, sizeof(meta), "%s", when[0] ? when : " ");
        }
        lv_obj_t *a = lv_label_create(txt);
        nl_ov_set_font(a);
        lv_label_set_text(a, meta);
        lv_obj_set_style_text_color(a, UI_TEXT_MUTED, 0);
        lv_label_set_long_mode(a, LV_LABEL_LONG_DOT);
        lv_obj_set_width(a, lv_pct(100));
    }
}

static bool news_step_locked(int delta, char *title, size_t title_len)
{
    if (title && title_len > 0) {
        title[0] = '\0';
    }
    if (!s_have_env || strcmp(s_env->card_type, "news_list") != 0 || strcmp(s_env->status, "ok") != 0 ||
        s_env->news_n <= 0 || !s_card) {
        return false;
    }
    const int n = s_env->news_n;
    const int cur = s_news_detail;
    int nxt = -1;
    if (delta == 0) {
        nxt = (cur < 0) ? 0 : cur;
    } else if (cur < 0) {
        if (delta < 0) {
            return false;
        }
        nxt = 0;
    } else if (delta < 0) {
        if (cur <= 0) {
            return false;
        }
        nxt = cur - 1;
    } else {
        if (cur + 1 >= n) {
            return false;
        }
        nxt = cur + 1;
    }
    if (s_minimized || lv_obj_has_flag(s_card, LV_OBJ_FLAG_HIDDEN)) {
        s_minimized = false;
        hide_mini_fab();
        set_drawer_for_skill(true);
        if (s_scrim) {
            lv_obj_clear_flag(s_scrim, LV_OBJ_FLAG_HIDDEN);
        }
        lv_obj_clear_flag(s_card, LV_OBJ_FLAG_HIDDEN);
    }
    const bool same = (nxt == cur && cur >= 0);
    s_news_detail = nxt;
    if (!same) {
        fill_list();
    }
    if (title && title_len > 0) {
        snprintf(title, title_len, "%s", s_env->news[nxt].title);
    }
    return true;
}

bool nodelio_skill_news_action(const char *op, char *out, size_t out_len)
{
    if (out && out_len > 0) {
        out[0] = '\0';
    }
    if (!op || !op[0]) {
        return false;
    }

    if (strcmp(op, "close") == 0 || strcmp(op, "dismiss") == 0) {
        bool open = false;
        {
            DisplayLockGuard lock(nl_ov_disp());
            open = lock && s_have_env && strcmp(s_env->card_type, "news_list") == 0;
        }
        if (open) {
            nodelio_skill_result_hide();
        }
        if (out && out_len > 0) {
            snprintf(out, out_len, open ? "已关闭新闻" : "新闻已经关了");
        }
        return true;
    }

    if (strcmp(op, "continue") == 0) {
        int idx = -1;
        uint32_t gen = 0;
        char id[NL_ENV_STR];
        id[0] = '\0';
        {
            DisplayLockGuard lock(nl_ov_disp());
            if (!lock || s_news_detail < 0 || s_news_detail >= s_env->news_n) {
                return false;
            }
            idx = s_news_detail;
            gen = s_news_gen;
            snprintf(id, sizeof(id), "%s", s_env->news[idx].id);
        }
        {
            DisplayLockGuard read_lock(nl_ov_disp());
            if (!read_lock) {
                return false;
            }
            if (s_news_read_progress.waiting()) {
                if (out && out_len > 0) {
                    snprintf(out, out_len,
                             "请先逐字朗读上一段原文，不要摘要。设备将在音频播完后请求续段，本轮不"
                             "要再调 continue。");
                }
                return true;
            }
            // A user interrupted the previous chunk: replay it instead of skipping
            // the unplayed suffix. Completed chunks advance exactly once.
            if (s_news_read_progress.unfinished()) {
                s_speak_off = s_speak_off_prev;
            }
            if (s_speak_body && strcmp(s_speak_id, id) == 0) {
                if (s_speak_off >= s_speak_len) {
                    if (out && out_len > 0) {
                        snprintf(
                            out, out_len,
                            s_speak_from_summary
                                ? "【完】摘要已全部给出。若要下一篇用 op=next；不要重复说已经读完。"
                                : "【完】本篇正文已全部给出。若要下一篇用 "
                                  "op=next；不要重复说已经读完。");
                    }
                    return true;
                }
                return news_format_chunk(out, out_len, false);
            }
        }
        return news_load_spoken(idx, gen, out, out_len, true);
    }

    if (strcmp(op, "read") == 0) {
        int idx = -1;
        uint32_t gen = 0;
        {
            DisplayLockGuard lock(nl_ov_disp());
            if (!lock || !news_step_locked(0, nullptr, 0)) {
                return false;
            }
            idx = s_news_detail;
            gen = s_news_gen;
        }
        return news_load_spoken(idx, gen, out, out_len, true);
    }

    const bool is_mark = strcmp(op, "mark_read") == 0 || strcmp(op, "mark") == 0;
    const bool is_skip = strcmp(op, "skip") == 0;
    const bool is_next = strcmp(op, "next") == 0 || is_skip;
    const bool is_prev = strcmp(op, "prev") == 0;
    if (!is_mark && !is_next && !is_prev) {
        return false;
    }

    char cur_id[NL_ENV_STR];
    cur_id[0] = '\0';
    int cur = -1;
    {
        DisplayLockGuard lock(nl_ov_disp());
        if (!lock || !s_have_env || strcmp(s_env->card_type, "news_list") != 0 || s_env->news_n <= 0) {
            return false;
        }
        cur = s_news_detail;
        if (cur < 0) {
            cur = 0;
            s_news_detail = 0;
        }
        if (cur < 0 || cur >= s_env->news_n) {
            return false;
        }
        snprintf(cur_id, sizeof(cur_id), "%s", s_env->news[cur].id);
    }

    (void)news_post_mark_read(cur_id);
    if (strcmp(s_speak_id, cur_id) == 0) {
        news_speak_clear();
    }

    int idx = -1;
    uint32_t gen = 0;
    bool want_body = is_next || is_prev;
    {
        DisplayLockGuard lock(nl_ov_disp());
        if (!lock) {
            return false;
        }
        int rm = -1;
        for (int i = 0; i < s_env->news_n; i++) {
            if (strcmp(s_env->news[i].id, cur_id) == 0) {
                rm = i;
                break;
            }
        }
        if (rm < 0) {
            return false;
        }
        if (is_mark) {
            news_remove_local(rm);
            s_news_detail = -1;
            fill_list();
            if (out && out_len > 0) {
                snprintf(out, out_len, "已标为已读");
            }
            return true;
        }
        /* next/skip：删掉当前后同下标即下一篇；prev：删后取前一篇 */
        const int prefer = is_prev ? (rm - 1) : rm;
        news_remove_local(rm);
        if (s_env->news_n <= 0) {
            s_news_detail = -1;
            fill_list();
            if (out && out_len > 0) {
                snprintf(out, out_len, is_prev ? "已标已读，没有上一篇了" : "已略过，没有下一篇了");
            }
            return true;
        }
        if (prefer < 0) {
            s_news_detail = -1;
            fill_list();
            if (out && out_len > 0) {
                snprintf(out, out_len, "已标已读，没有上一篇了");
            }
            return true;
        }
        s_news_detail = prefer < s_env->news_n ? prefer : s_env->news_n - 1;
        if (s_minimized || lv_obj_has_flag(s_card, LV_OBJ_FLAG_HIDDEN)) {
            s_minimized = false;
            hide_mini_fab();
            set_drawer_for_skill(true);
            if (s_scrim) {
                lv_obj_clear_flag(s_scrim, LV_OBJ_FLAG_HIDDEN);
            }
            lv_obj_clear_flag(s_card, LV_OBJ_FLAG_HIDDEN);
        }
        fill_list();
        idx = s_news_detail;
        gen = s_news_gen;
    }
    if (!want_body || idx < 0) {
        return out && out[0];
    }
    /* 小缓冲=本地旁路：只切详情，不拉朗读段（避免无声跟读滚屏） */
    if (out_len > 0 && out_len < 512) {
        if (out) {
            snprintf(out, out_len, "%s", s_env->news[idx].title);
        }
        return true;
    }
    return news_load_spoken(idx, gen, out, out_len, true);
}

bool nodelio_skill_news_prepare_read(void)
{
    DisplayLockGuard lock(nl_ov_disp());
    if (!lock) {
        return false;
    }
    return news_step_locked(0, nullptr, 0);
}

bool nodelio_skill_news_local_speak(const char *op, char *plain_out, size_t plain_len)
{
    if (plain_out && plain_len > 0) {
        plain_out[0] = '\0';
    }
    if (!op || !op[0]) {
        return false;
    }
    const bool is_cont = strcmp(op, "continue") == 0;
    const bool is_read = strcmp(op, "read") == 0;
    if (!is_cont && !is_read) {
        return false;
    }

    int idx = -1;
    uint32_t gen = 0;
    char id[NL_ENV_STR];
    id[0] = '\0';
    {
        DisplayLockGuard lock(nl_ov_disp());
        if (!lock) {
            return false;
        }
        if (is_read) {
            if (!news_step_locked(0, nullptr, 0)) {
                return false;
            }
        } else if (s_news_detail < 0 || s_news_detail >= s_env->news_n) {
            return false;
        }
        idx = s_news_detail;
        gen = s_news_gen;
        snprintf(id, sizeof(id), "%s", s_env->news[idx].id);
    }

    if (is_cont) {
        /* 本地 TTS 停即续；勿套 MCP 的 8s 防连调闸门（短文会误杀续读） */
        if (s_speak_body && strcmp(s_speak_id, id) == 0) {
            if (s_speak_off >= s_speak_len) {
                if (plain_out && plain_len > 0) {
                    snprintf(plain_out, plain_len, "本篇已经读完。可以说下一篇。");
                }
                return plain_out && plain_out[0];
            }
            return news_format_plain_chunk(plain_out, plain_len, false);
        }
    }

    /* read 或 continue 无缓存：拉正文后吐 plain */
    char *heap = nullptr;
    const char *view = nullptr;
    bool from_summary = true;
    if (!news_pull_article_text(idx, &heap, &view, &from_summary) || !view || !view[0]) {
        if (plain_out && plain_len > 0) {
            snprintf(plain_out, plain_len, "这篇正文暂时打不开。");
        }
        return plain_out && plain_out[0];
    }
    news_apply_body_label(idx, gen, view, from_summary);
    news_speak_set(id, s_env->news[idx].title, view, from_summary);
    if (heap) {
        heap_caps_free(heap);
    }
    return news_format_plain_chunk(plain_out, plain_len, true);
}

bool nodelio_skill_try_news_pick_phrase(const char *text, bool *keep_cloud_tts)
{
    if (keep_cloud_tts) {
        *keep_cloud_tts = false;
    }
    if (!text || !text[0] || !nodelio_skill_news_ui_active() || !s_env) {
        return false;
    }
    if (strstr(text, "不要读") || strstr(text, "不用读") || strstr(text, "别读") ||
        strstr(text, "不读") || strstr(text, "不朗读") || strstr(text, "不要朗读") ||
        strstr(text, "不用朗读")) {
        return false;
    }
    int ord = parse_ordinal_zh(text);
    if (ord <= 0 && (strstr(text, "第一条") || strstr(text, "首条") || strstr(text, "头条"))) {
        ord = 1;
    }
    if (ord <= 0) {
        return false;
    }
    /* 须像在点新闻：有「条/篇/个/新闻」或「打开/读/念」 */
    const bool news_ish =
        strstr(text, "条") || strstr(text, "篇") || strstr(text, "新闻") || strstr(text, "个") ||
        strstr(text, "打开") || strstr(text, "读") || strstr(text, "念") || strstr(text, "听");
    if (!news_ish) {
        return false;
    }
    if (s_env->news_n <= 0) {
        nl_ov_toast("没有新闻");
        nodelio_panel_ui_announce_spoken("现在没有新闻可打开。");
        return true;
    }
    if (ord > s_env->news_n) {
        char tip[48];
        snprintf(tip, sizeof(tip), "只有 %d 条", s_env->news_n);
        nl_ov_toast(tip);
        nodelio_panel_ui_announce_spoken("没有这个序号，请再说第几条。");
        return true;
    }

    const bool want_read =
        strstr(text, "读") || strstr(text, "念") || strstr(text, "朗读") || strstr(text, "听一下");
    {
        DisplayLockGuard lock(nl_ov_disp());
        if (!lock) {
            return false;
        }
        s_news_detail = ord - 1;
        if (s_minimized || (s_card && lv_obj_has_flag(s_card, LV_OBJ_FLAG_HIDDEN))) {
            s_minimized = false;
            hide_mini_fab();
            set_drawer_for_skill(true);
            if (s_scrim) {
                lv_obj_clear_flag(s_scrim, LV_OBJ_FLAG_HIDDEN);
            }
            if (s_card) {
                lv_obj_clear_flag(s_card, LV_OBJ_FLAG_HIDDEN);
            }
        }
        fill_list();
    }
    ESP_LOGI("nl_news", "voice pick ord=%d read=%d title=%s", ord, want_read ? 1 : 0,
             s_env->news[ord - 1].title);

    if (!want_read) {
        char tip[64];
        snprintf(tip, sizeof(tip), "第 %d 条", ord);
        nl_ov_toast(tip);
        return true;
    }

    /* 开详情后短 detect 促云端调 self.skill.news（正常长 TTS） */
    Application::GetInstance().StartLocalNewsRead("read");
    if (keep_cloud_tts) {
        *keep_cloud_tts = true; /* 等 MCP 正文 TTS，勿 Suppress */
    }
    return true;
}
