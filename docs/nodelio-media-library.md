# NodeLio 曲库：内存卡来源与列表记忆

面板曲库（`skill_result_ui.cc` + `sd_library.cc` + `media_player.cc`）的两项能力说明。
适用板型：`sila_p4c5_62`（TF Slot0）与 `sila_86`（无卡，芯片自动隐藏）。

## 1. 内存卡来源

| 层 | 位置 | 职责 |
|---|---|---|
| 板级 | `Board::GetSdCardPins()` | 只有接卡的板返回有效引脚；其它板保持全 -1 |
| 卡库 | `main/nodelio/skill/sd_library.cc` | 挂载/掉电、递归扫描、分批、`sd://` ↔ VFS 路径 |
| 播放 | `main/notify/notify_player.cc` | `StartFile()` + `LocalFileHttp` + 流式 WAV |
| 应用层 | `Application::PlayLocalAudioFile()` | 与 `PlayHttpAudio` 共用 Notifying 播放槽 |
| UI | `skill_result_ui.cc` | 来源 chip、批次、空态、`换一批` 语义 |
| 语音/MCP | `offline_cmd_map.c` / `nodelio_mcp.cc` | 「打开内存卡」「放内存卡里的歌」同一条路径 |
| 文件应用 | `sd_files_ui.cc` + `nodelio_sd_list_dir` | 应用页「文件」= 目录浏览；点音频/图片再进播放或相册，**不是**曲库 |

### 约定

- 应用「文件」与「面板曲库」分开：前者浏览目录，后者才是 `music.search` / `music.sd` 歌单。
- 曲库只列 `mp3 / wav / m4a`。列表里出现的文件必须能播：flac/aac/ogg 目前没有可用的分帧解析器，不列出。
- 扫描上限 `NL_SD_MAX_FILES`=200、深度 3；每批 24 首（与 `NL_ENV_TRACK_MAX` 对齐）。
- 挂载点 `/sdcard`，`playback_url` 形如 `sd:///sdcard/Music/a.mp3`，播放前由
  `nodelio_sd_resolve_url()` 还原为 VFS 路径。
- 按需挂载：进入内存卡来源才挂卡；`PWRn`（低有效）在未挂载时断电。挂载失败不 abort、不格式化。
- 内存卡曲库的 `app_id` 是本地 `music.sd`；切回在线来源时刷新按钮强制改发 `music.search`。

### 未验证

- `SD_CARD_PWR_PIN`(51) 的有效电平、`SD_CARD_CD_PIN`(52) 极性；
- Slot0 与 Hosted Slot1 并发、以及 20MHz 4-bit 下的播放稳定性；
- 首次上机需要 `idf.py monitor` 看 `nl_sd` 的 `mounted/scan` 日志。

## 2. 列表记忆（「播放第一首」不再重新搜歌）

现象：小智打开曲库拿到列表后，用户说「播放第一首」，云端会再调一次
`self.skill.invoke(music.search)`；服务端返回新一批结果，于是播的是**新列表**的第一首。

修复点（四道保险，任一命中即不再发请求）：

1. **本地序号解析**：`music_keyword_ordinal()` 识别 `播放第一首 / 放第三首 / 来第2个`，
   命中 `nodelio_skill_media_play_index()` 直接在当前列表点播。
2. **MCP 显式接口**：`self.skill.transport` 新增
   - `op=play index=N`：按 1-based 序号播放当前列表；
   - `op=list`：返回当前歌单曲名（模型回答「第一首是什么」前先读它）。
3. **语音旁路**：`nodelio_skill_try_music_pick_phrase()` 在 STT 本地路由里处理
   「播放第一首」并结束本轮会话，云端不再参与。
4. **工具描述**：`self.skill.invoke` / `self.skill.transport` 的描述明确
   「列表已开时按序号播放，禁止重新 invoke music.search」。

模型每轮只拿到 `spoken` 摘要（最多 5 个曲名），所以设备侧必须自己是列表的唯一权威：
`s_env` 在卡片收起后仍保留（`close_list_keep_player`），点播放条可恢复列表与播放位置。