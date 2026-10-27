/* 由 skill_result_ui.cc 在 fill_list 定义前包含。依赖该文件的静态状态。 */

static void fill_list(void);
static bool news_step_locked(int delta, char *title, size_t title_len);
static void news_fetch_article(const news_job_t *job);
bool nodelio_skill_news_action(const char *op, char *out, size_t out_len);
static bool news_load_spoken(int idx, uint32_t gen, char *out, size_t out_len, bool reset_chunk);
static void news_speak_clear(void);
static int32_t news_zoom_scale(void);
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
static int64_t s_chunk_sent_us = 0;
/** true=仅摘要/标题，勿让模型声称「全文已读完」 */
static bool s_speak_from_summary = false;
/** 分片长达 ~2800B，仅在 chunk 边界滚几乎看不出跟读；定时按语速估算进度 */
static esp_timer_handle_t s_news_follow_tmr = nullptr;
static void news_follow_stop_timer(void);
static void news_follow_ensure_timer(void);
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
    news_follow_stop_timer();
    if (s_speak_body) {
        heap_caps_free(s_speak_body);
        s_speak_body = nullptr;
    }
    s_speak_len = 0;
    s_speak_off = 0;
    s_speak_off_prev = 0;
    s_speak_id[0] = '\0';
    s_speak_title[0] = '\0';
    s_chunk_sent_us = 0;
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

static size_t news_follow_estimate_off(void)
{
    if (!s_speak_body || s_chunk_sent_us == 0) {
        return s_speak_off_prev;
    }
    /* 约 4.5 汉字/秒 → ~14 UTF-8 字节/秒；宁肯略慢，别超前 TTS */
    const int64_t elapsed_us = esp_timer_get_time() - s_chunk_sent_us;
    if (elapsed_us <= 0) {
        return s_speak_off_prev;
    }
    size_t adv = static_cast<size_t>((elapsed_us * 14) / 1000000LL);
    size_t off = s_speak_off_prev + adv;
    if (off > s_speak_off) {
        off = s_speak_off;
    }
    if (off > s_speak_len) {
        off = s_speak_len;
    }
    return news_utf8_floor(s_speak_body, off);
}

static void news_follow_timer_cb(void * /*arg*/)
{
    Application::GetInstance().Schedule([]() {
        if (!s_speak_body || s_news_detail < 0) {
            news_follow_stop_timer();
            return;
        }
        news_scroll_to_byte(news_follow_estimate_off());
        /* 本段已估到末尾且全文念完 → 停表 */
        if (s_speak_off >= s_speak_len && news_follow_estimate_off() >= s_speak_off) {
            news_follow_stop_timer();
        }
    });
}

static void news_follow_stop_timer(void)
{
    if (s_news_follow_tmr) {
        esp_timer_stop(s_news_follow_tmr);
    }
}

static void news_follow_ensure_timer(void)
{
    if (!s_news_follow_tmr) {
        const esp_timer_create_args_t args = {
            .callback = &news_follow_timer_cb,
            .arg = nullptr,
            .dispatch_method = ESP_TIMER_TASK,
            .name = "nl_news_flw",
            .skip_unhandled_events = true,
        };
        if (esp_timer_create(&args, &s_news_follow_tmr) != ESP_OK) {
            s_news_follow_tmr = nullptr;
            return;
        }
    }
    esp_timer_stop(s_news_follow_tmr);
    esp_timer_start_periodic(s_news_follow_tmr, 1200 * 1000);
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
    return end;
}

static void news_speak_set(const char *id, const char *title, const char *body, bool from_summary)
{
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
    const int32_t zscale = news_zoom_scale();
    const lv_coord_t vis_y = (pos.y * (lv_coord_t)zscale) / (lv_coord_t)LV_SCALE_NONE;
    lv_coord_t y = lv_obj_get_y(s_news_body_lbl) + vis_y - (view_h > 0 ? view_h / 4 : 40);
    if (y < 0) {
        y = 0;
    }
    const lv_coord_t max_y = lv_obj_get_scroll_bottom(s_card_list) + lv_obj_get_scroll_y(s_card_list);
    if (max_y > 0 && y > max_y) {
        y = max_y;
    }
    lv_obj_scroll_to_y(s_card_list, y, LV_ANIM_ON);
}

static void news_scroll_follow(void)
{
    news_scroll_to_byte(news_follow_estimate_off());
    news_follow_ensure_timer();
}

static bool news_format_chunk(char *out, size_t out_len, bool with_title)
{
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
    const size_t end = news_utf8_chunk_end(s_speak_body, s_speak_off, NEWS_MCP_CHUNK);
    const size_t take = end > s_speak_off ? end - s_speak_off : 0;
    const bool more = end < s_speak_len;
    char *piece = static_cast<char *>(heap_caps_malloc(take + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!piece) {
        piece = static_cast<char *>(malloc(take + 1));
    }
    if (!piece) {
        return false;
    }
    memcpy(piece, s_speak_body + s_speak_off, take);
    piece[take] = '\0';
    s_speak_off_prev = s_speak_off;
    s_speak_off = end;
    s_chunk_sent_us = esp_timer_get_time();
    news_scroll_follow();
    const char *tail_more =
        "\n【续】请先逐字朗读以上原文，不要摘要、不要说读完。念完后再调 op=continue。同一轮不要连续调用。";
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

/** 详情正文缩放档：0=现状，1≈125%，2≈150%（会话内记住）。 */
static uint8_t s_news_zoom = 0;
static lv_obj_t *s_news_zoom_hint = nullptr;

static int32_t news_zoom_scale(void)
{
    static const int32_t k_scale[] = {UI_NEWS_ZOOM_SCALE_0, UI_NEWS_ZOOM_SCALE_1, UI_NEWS_ZOOM_SCALE_2};
    if (s_news_zoom > UI_NEWS_ZOOM_MAX) {
        return UI_NEWS_ZOOM_SCALE_0;
    }
    return k_scale[s_news_zoom];
}

static const char *news_zoom_name(void)
{
    static const char *k_name[] = {"标准", "较大", "更大"};
    if (s_news_zoom > UI_NEWS_ZOOM_MAX) {
        return k_name[0];
    }
    return k_name[s_news_zoom];
}

/** 用缩放放大正文（无更大中文字库）；缩窄换行宽 + pad 补布局高度。 */
static void news_apply_body_zoom(void)
{
    if (!s_news_body_lbl || !lv_obj_is_valid(s_news_body_lbl) || !s_card_list) {
        return;
    }
    const int32_t scale = news_zoom_scale();
    lv_obj_update_layout(s_card_list);
    lv_coord_t avail = lv_obj_get_content_width(s_card_list);
    if (avail < 80) {
        avail = 400;
    }
    lv_coord_t wrap_w = (avail * (lv_coord_t)LV_SCALE_NONE) / (lv_coord_t)scale;
    if (wrap_w < 60) {
        wrap_w = 60;
    }
    lv_obj_set_width(s_news_body_lbl, wrap_w);
    lv_obj_set_height(s_news_body_lbl, LV_SIZE_CONTENT);
    lv_obj_set_style_transform_pivot_x(s_news_body_lbl, 0, 0);
    lv_obj_set_style_transform_pivot_y(s_news_body_lbl, 0, 0);
    lv_obj_set_style_transform_scale_x(s_news_body_lbl, scale, 0);
    lv_obj_set_style_transform_scale_y(s_news_body_lbl, scale, 0);
    lv_obj_add_flag(s_news_body_lbl, LV_OBJ_FLAG_OVERFLOW_VISIBLE);
    lv_obj_update_layout(s_news_body_lbl);
    const lv_coord_t content_h = lv_obj_get_height(s_news_body_lbl);
    lv_coord_t extra = 0;
    if (scale > (int32_t)LV_SCALE_NONE && content_h > 0) {
        extra = (content_h * (scale - (int32_t)LV_SCALE_NONE)) / (lv_coord_t)LV_SCALE_NONE;
    }
    lv_obj_set_style_pad_bottom(s_news_body_lbl, 6 + extra, 0);
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
    if (idx < 0 || idx >= s_env->news_n || !s_env->news[idx].id[0]) {
        return false;
    }
    const char *title = s_env->news[idx].title;
    const char *id = s_env->news[idx].id;
    if (!reset_chunk && s_speak_body && strcmp(s_speak_id, id) == 0) {
        return news_format_chunk(out, out_len, false);
    }
    char *heap = nullptr;
    const char *view = nullptr;
    bool from_summary = true;
    if (!news_pull_article_text(idx, &heap, &view, &from_summary) || !view || !view[0]) {
        if (out && out_len > 0) {
            snprintf(out, out_len, "%s\n（正文暂不可用，请稍后重试，不要说已经读完）",
                     title && title[0] ? title : "这篇新闻");
        }
        return out && out[0];
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

static void fill_news(void)
{
    set_hdr_mid_icon(MATERIAL_SYMBOLS_KEYBOARD_ARROW_DOWN);
    s_news_gen += 1;
    news_speak_clear();
    /* 不在此清 article 缓存：fill 可能与 HTTP 单飞并发；错 id 由 pull 内比对替换 */
    news_body_heap_clear();
    s_news_body_lbl = nullptr;
    s_news_zoom_hint = nullptr;
    lv_obj_clean(s_card_list);
    lv_obj_add_flag(s_card_list, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(s_card_list, LV_DIR_VER);
    lv_obj_scroll_to_y(s_card_list, 0, LV_ANIM_OFF);

    if (strcmp(s_env->status, "ok") != 0 || s_env->news_n <= 0) {
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
        if (s_card_sub) {
            char sub[32];
            snprintf(sub, sizeof(sub), "详情 %d/%d", s_news_detail + 1, s_env->news_n);
            lv_label_set_text(s_card_sub, sub);
        }
        lv_obj_set_style_pad_row(s_card_list, 12, 0);

        lv_obj_t *back = lv_label_create(s_card_list);
        nl_ov_set_font(back);
        lv_label_set_text(back, "返回列表");
        lv_obj_set_style_text_color(back, UI_ACCENT_SOFT, 0);
        lv_obj_set_style_pad_bottom(back, 4, 0);
        lv_obj_add_flag(back, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(back, on_news_back, LV_EVENT_CLICKED, nullptr);

        lv_obj_t *ttl = lv_label_create(s_card_list);
        nl_ov_set_font(ttl);
        lv_label_set_text(ttl, n->title);
        lv_obj_set_width(ttl, lv_pct(100));
        lv_label_set_long_mode(ttl, LV_LABEL_LONG_WRAP);
        lv_obj_set_style_text_color(ttl, UI_TEXT, 0);
        lv_obj_set_style_pad_ver(ttl, 4, 0);

        lv_obj_t *nav = lv_obj_create(s_card_list);
        lv_obj_remove_style_all(nav);
        lv_obj_set_width(nav, lv_pct(100));
        lv_obj_set_height(nav, LV_SIZE_CONTENT);
        lv_obj_set_style_pad_ver(nav, 8, 0);
        lv_obj_set_flex_flow(nav, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(nav, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_t *prev = lv_label_create(nav);
        nl_ov_set_font(prev);
        lv_label_set_text(prev, "上一篇");
        lv_obj_set_style_text_color(prev, UI_ACCENT_SOFT, 0);
        lv_obj_add_flag(prev, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(prev, on_news_nav, LV_EVENT_CLICKED, (void *)(intptr_t)-1);
        lv_obj_t *next = lv_label_create(nav);
        nl_ov_set_font(next);
        lv_label_set_text(next, "下一篇");
        lv_obj_set_style_text_color(next, UI_ACCENT_SOFT, 0);
        lv_obj_add_flag(next, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(next, on_news_nav, LV_EVENT_CLICKED, (void *)(intptr_t)1);

        /* 字号：最小=现状；A+ 放大到较大/更大 */
        lv_obj_t *zoom = lv_obj_create(s_card_list);
        lv_obj_remove_style_all(zoom);
        lv_obj_set_width(zoom, lv_pct(100));
        lv_obj_set_height(zoom, LV_SIZE_CONTENT);
        lv_obj_set_style_pad_ver(zoom, 4, 0);
        lv_obj_set_flex_flow(zoom, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(zoom, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_column(zoom, 16, 0);
        lv_obj_t *zm = lv_label_create(zoom);
        nl_ov_set_font(zm);
        lv_label_set_text(zm, "A-");
        lv_obj_set_style_text_color(zm, UI_ACCENT_SOFT, 0);
        lv_obj_add_flag(zm, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_ext_click_area(zm, UI_EXT_CLICK);
        lv_obj_add_event_cb(zm, on_news_zoom, LV_EVENT_CLICKED, (void *)(intptr_t)-1);
        s_news_zoom_hint = lv_label_create(zoom);
        nl_ov_set_font(s_news_zoom_hint);
        lv_label_set_text(s_news_zoom_hint, news_zoom_name());
        lv_obj_set_style_text_color(s_news_zoom_hint, UI_TEXT_MUTED, 0);
        lv_obj_t *zp = lv_label_create(zoom);
        nl_ov_set_font(zp);
        lv_label_set_text(zp, "A+");
        lv_obj_set_style_text_color(zp, UI_ACCENT_SOFT, 0);
        lv_obj_add_flag(zp, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_ext_click_area(zp, UI_EXT_CLICK);
        lv_obj_add_event_cb(zp, on_news_zoom, LV_EVENT_CLICKED, (void *)(intptr_t)1);

        s_news_body_lbl = lv_label_create(s_card_list);
        nl_ov_set_font(s_news_body_lbl);
        /* 勿先贴 RSS 摘要（带「…」），否则长文加载失败时用户会以为详情只有概述 */
        lv_label_set_text(s_news_body_lbl, "正在加载详情…");
        lv_label_set_long_mode(s_news_body_lbl, LV_LABEL_LONG_WRAP);
        lv_obj_set_style_text_color(s_news_body_lbl, UI_TEXT_DIM, 0);
        lv_obj_set_style_pad_top(s_news_body_lbl, 6, 0);
        news_apply_body_zoom();
        news_enqueue_article(s_news_detail);
        return;
    }

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
        if (s_chunk_sent_us != 0 && esp_timer_get_time() - s_chunk_sent_us < 8000000) {
            if (out && out_len > 0) {
                snprintf(out, out_len,
                         "上一段原文还没念完。请先逐字朗读上一段，不要摘要，不要换篇，不要说读完。念完后再调用 continue。");
            }
            return true;
        }
        if (s_speak_body && strcmp(s_speak_id, id) == 0) {
            if (s_speak_off >= s_speak_len) {
                if (out && out_len > 0) {
                    snprintf(out, out_len,
                             s_speak_from_summary
                                 ? "【完】摘要已全部给出。若要下一篇用 op=next；不要重复说已经读完。"
                                 : "【完】本篇正文已全部给出。若要下一篇用 op=next；不要重复说已经读完。");
                }
                return true;
            }
            return news_format_chunk(out, out_len, false);
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
    return news_load_spoken(idx, gen, out, out_len, true);
}
