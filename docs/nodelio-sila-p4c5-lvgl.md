# sila-p4c5-62 LVGL / 图片预览要点

适用：`main/boards/sila/sila-p4c5-62`（ESP32-P4 + MIPI DSI，逻辑 1280×452）。  
本文只记录对本板成立的约束与做法；其它芯片/RGB 直出屏的跑分经验若与本文冲突，以本板为准。

## 显示基线（保持）

| 项 | 本板约定 |
|---|---|
| 面板色深 | DPI **RGB888** 全程；不要把 LVGL 显示改成 RGB565 以「省带宽」——flush 仍要对齐面板 |
| 帧缓冲 | `dpi.num_fbs = 2` |
| LVGL 绘制缓冲 | 双 partial，默认约 **1/4 物理屏**像素 ×2，放在 **PSRAM**（`buff_spiram`）；非 full_refresh / direct_mode |
| 旋转 | `sw_rotate` + **`CONFIG_LVGL_PORT_ENABLE_PPA=y`**（输出旋转）；禁止关掉 port PPA 指望 CPU 旋转撑住 |
| Vendor 绘制 PPA | `CONFIG_LV_USE_PPA=n` / `LV_USE_PPA_IMG=n`；板级 `NODELIO_P4_PPA_FILL` 仅接管大块不透明 fill，与 port 旋转客户端分离 |
| OS / draw unit | `CONFIG_LV_OS_NONE=y`，`DRAW_SW_DRAW_UNIT_CNT=1`；不启双 draw unit |
| XIP | `CONFIG_SPIRAM_XIP_FROM_PSRAM=y`（`sdkconfig.defaults.esp32p4`） |

物理 452×1280、逻辑旋转 90°、时序与内存粗算见板级 [README](../main/boards/sila/sila-p4c5-62/README.md)。

## 调度

- LVGL 任务 **钉 core 1**（`lvgl_port` `task_affinity = 1`），给 Hosted / 音频留出 core 0 调度空间。
- 不要为「跑分」打开 LVGL FreeRTOS OS + 多 draw unit：本产品 core 0 有业务负载时，尾延迟往往更差。

## 图片预览路径（卡顿优先改这里）

```text
JPEG(文件/HTTP) → jpeg_to_image_fit(视口/瓦片尺寸) → RGB565 缓冲
    → lv_image 1:1 上屏（禁止常态 STRETCH/CONTAIN/COVER 每帧重采样）
放大：异步重解 + 不足目标时一次性 nearest 放大 → 仍 1:1 上屏
```

要点：

1. **按目标区域解码**，不要解原分辨率再靠 LVGL 缩放。入口：`jpeg_to_image_fit()`（`display/lvgl_display/jpg/jpeg_to_image.*`）。
2. **位图已是 RGB565**；面板仍是 RGB888。图片侧用 565 省内存与混合带宽，不等于改面板色深。
3. **全屏预览**（`ui_image_preview`）：`set_jpeg` 锁外 decode_fit（100% 记为 base）、锁内 1:1；侧栏缩放目标为 `base×zoom/256`（相对图，不是撑满视口）；源图不够时 **一次性** 放大到该目标，禁止每帧 `STRETCH`。
4. **无双指捏合**：触摸驱动只向 LVGL 喂单点 `POINTER`（AXS15260 帧里虽有 count，固件未实现多点 indev）。
5. **SD 相册 / 首页相册**：同样走 fit；首页瓦片按 content 宽高限制解码后居中 1:1。
6. **硬解**：`CONFIG_XIAOZHI_ENABLE_HARDWARE_JPEG_DECODER=y`（P4 `config.json`）。仅在 **无需缩小** 时经 `jpeg_to_image()` 走硬解；需要缩小仍用 `esp_new_jpeg` scale。

## 后台播歌（单卡，非双应用并排）

- HTTP 曲库播放时可切首页 / 设置 / 应用列表；打开日历、客服、新闻等**非新音频流**技能 **不停歌**。
- 仅电台 / 播客等会 `preempt` 停歌再播新流。
- 技能 UI 仍是**单卡** `s_env`；播歌中换卡时歌单可 stash，再点曲库 `try_reopen_music_list` 恢复，不重新搜歌切曲。
- 仅播放条可见时不挡应用列表（scrim 只挡技能卡打开时）。

## 不要做（对本板）

- 整屏 LVGL 改 RGB565、关闭 `LVGL_PORT_ENABLE_PPA`、打开 `LV_USE_PPA`
- 依赖超大 `CONFIG_LV_CACHE_DEF_SIZE` / image header cache：预览走手工 `lv_image_dsc_t`，不经 LVGL 文件解码器缓存
- 未测就把 1/4 屏 RGB888 draw buffer 强塞内部 SRAM
- 双 draw unit / `LV_USE_OS=FREERTOS` 当产品默认
- 双技能卡并排、双指捏合（本板当前不支持）

## 相关代码

- 板显示与绑核：`main/boards/sila/sila-p4c5-62/sila_p4c5_62_board.cc`
- 解码：`main/display/lvgl_display/jpg/jpeg_to_image.c`
- 预览壳：`main/nodelio/ui_image_preview.*`
- 客服 / SD / 首页：`skill_support_ui.cc`、`sd_photo_ui.cc`、`overlay_home_photo.cc`
- 曲库 reopen / stash / preempt：`skill_result_ui.*`、`skill_runtime.*`、`overlay_apps.cc`

## 验收（实机）

- 客服大图：侧栏放大/缩小有可见变化，可拖动画布
- 播歌 → 设置 → 应用 → 再点曲库：同一首继续，列表回来
- 播歌 → 打开日历：不停歌；再点曲库恢复列表
- 播歌 → 打开电台/播客：停歌并切新流
- 仅播放条时能点应用列表
- Hosted 联网 + 语音并发下点图，无长时间卡死
