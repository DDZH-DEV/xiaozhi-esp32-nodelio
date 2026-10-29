# SILA-86 on xiaozhi-esp32 v2.5.2 (NodeLio stage K)

## Status
- Source: official tag v2.5.2 → `D:\xz25` (branch `nodelio-sila86`)
- **Firmware root**: this tree. `D:\xz` = 1.5.6 pin reference only.
- **IDF 6.1**: `D:\Espressif\frameworks\esp-idf-v6.1` + `idf6.1_py3.11_env`
- **K0–K2 done**: board + chat + `main/nodelio/` overlay
- **K3 in progress**: register / layout HTTP / `panel/command` client; LVGL touch shell still stubbed

## Layout
| Path | Role |
|------|------|
| `main/boards/sila/sila-86/` | BSP (ST7701 / FT5x06 / ES8311) |
| `main/nodelio/` | NodeLio client (NVS, register, MQTT notify, layout, command) |
| Xiaozhi stack | Wake / chat / personal server |

WiFi SoftAP stays with Xiaozhi. NodeLio does **not** own STA.

## Build / flash
```bat
cd /d D:\xz25
nodelio-build.bat
nodelio-flash.bat
```

## AFE 启动后的 AES / MQTT 内存故障（2026-10-29）

实机日志在 AFE 初始化后显示内部堆 `free=9795 largest=7168`，随后发送
1859 / 1694 字节的 MCP 工具列表出现 `esp-aes: Failed to allocate memory`、
TLS `-0x0084` 和 MQTT 断线。此时云端无法完整发现新闻和 DSH 工具，不能先归因于应用未安装。

[ESP-IDF 6.1 AES DMA 实现](https://github.com/espressif/esp-idf/blob/release/v6.1/components/mbedtls/port/aes/dma/esp_aes_dma_core.c)
会为不满足 DMA/对齐要求的缓冲申请 DMA 中转内存，S3 按最多 1600 字节分块。
`MALLOC_CAP_INTERNAL` 的最大块不等于可供 DMA 使用的最大块；PSRAM 有余量、
MCP 小于 2KB 都不能保证硬件 AES 分配成功。原 MCP 预算仅保留为消息内存上限。

本板在 `config.json` 中使用 `CONFIG_MBEDTLS_HARDWARE_AES=n`，由 mbedTLS/PSA
执行软件 AES，TLS 和 UDP 音频加密仍保留。没有修改其他板型、SDK 源码或生成配置。
软件加密的 CPU 占用、播音稳定性和功耗需要实机测量；本次不新增后台任务或定时轮询。

构建入口：

- 标准入口：`python scripts/build.py sila/sila-86 --name sila-86`，自动应用板型配置。
- `nodelio-build.bat`：在构建时迁移已有配置的 AES 选项。
- 直接使用 `idf.py build`：先 `idf.py menuconfig`，按 `/` 搜索
  `MBEDTLS_HARDWARE_AES`，关闭并保存，再 `idf.py reconfigure`、`idf.py build`。
  已有 sdkconfig 会覆盖 defaults，所以仅更新源代码不够。CMake 会明确拒绝本板仍开启
  硬件 AES 的配置，避免再次生成带有已知故障的固件。

下一次实机验证：

1. MQTT 初始化应出现 `TLS AES=software`，同时打印 internal / DMA free 和 largest。
2. AFE 初始化之后，所有 `tools/list` 分页应能完成；不再出现 AES 分配失败、TLS 写失败或
   `MCP response not sent`。若仍失败，提供这些 DMA 数值和完整启动日志。
3. 验证新闻正文工具调用和朗读、DSH 查看任务/续聊/取消、打断及重连。
4. MQTT/UDP 和适用的 WebSocket 路径分别验证，检查软件 AES 下连续播音、唤醒和息屏功耗。

## Bind NodeLio SERVER（LVGL，非 menuconfig）
1. 烧录后点右下角绿色 **「家」**
2. **设置** 页填：HTTP/HTTPS、主机、端口、绑定码 → **保存并连接**（会重启）
3. **首页** 显示 APP 下发的 layout 控件；点块走 `panel/command`

未绑定时只显示 FAB，需点「家」进设置（避免开机自动建壳栈溢出重启）。menuconfig 的 `NODELIO_SERVER_URL/OWNER` 仅可选出厂种子。

日志：`nl_bridge: NodeLio panel up device_id=WP-…` / `nl_overlay: layout vN applied`

## Voice → NodeLio（MCP）
对话时个人服可调设备 MCP：
- `self.nodelio.list_controls` — 当前布局控件 JSON
- `self.nodelio.control` — `phrase`：开灯/关灯/回家/出门/打开窗帘/关闭窗帘；或 `widget_title`+`value`

均经 SERVER `panel/command`，不直连节点。

## Next
1. 实机：绑定 → 触屏控家（勾 K3）→ 语音「开灯/回家」（勾 K5）
2. K4 断网口令（ESP-SR，与小智 AFE 互斥）

## 冷启动首轮回复无声排查（2026-10-29）

13:06:43 构建的日志中，MQTT 工具列表分页全部完成，首轮有 `cloud tts start` 和
“你好呀～”等文字，输出音量为 70，但没有 `cloud audio rx`、`pcm out` 或 UDP 解密错误。
这说明日志尚未显示音频进入播放队列，不能据此认定是功放静音。

墙板为节省内部堆跳过唤醒录音编码，而首次聆听很快转为 Speaking，停止麦克风上传。
原 MQTT 实现仅连接本地 UDP socket 后就允许发送唤醒 detect，存在尚未发出任何 UDP
包便要求服务器回复的时序缺口。现在每次新建 MQTT/UDP 通道先通过正常加密发送路径
发出一个 60 ms 静音包（8 字节 Opus + 16 字节包头），成功后才向应用报告打开成功。
该行为放在 MQTT 传输层，覆盖冷启动、重连和新会话；不恢复整段唤醒录音编码，不增加
常驻任务、轮询或周期性保活。WebSocket 不经过此逻辑。

这针对当前证据中最可疑的回传路径缺口，仍需实机确认，不能把本地发送成功当作服务端
已经收到。编译后断电重启，第一次唤醒后先停顿、等待问候，不要靠继续说话补上行包：

1. 应先出现 `UDP audio primed: 8 bytes, 60 ms silence`，再出现唤醒 detect。
2. 问候时应出现 `UDP first audio rx`、`cloud audio rx #1` 和 `pcm out #0`，并听到声音。
3. 再验证连续对话、打断、会话关闭后再次唤醒、息屏断网恢复和适用的 AEC 模式。
4. 若只有 primed 而没有 first audio rx，继续查服务端 UDP 回传、网络丢包及接收任务；
   若已有 PCM 日志仍无声，再查 codec/I2S 输出。

本次以 FFmpeg 原生 Opus 和 libopus 两个解码器验证静音载荷，均解出 60 ms 单声道音频，
PCM 峰值为 0。按要求未编译、烧录固件，未进行实机验证。

## 新闻应用与朗读排查（2026-10-29）

新闻应用安装、文章 HTTP 获取、云端 MCP 工具发现、TTS 播放是不同环节。
`open_by_query ... news.latest` 和 `panel/news/article ... HTTP 200` 说明前两步成功，
不能据此认定模型已经拿到正文。判断工具是否调用，应看
`nl_mcp: tools/call self.skill.news op=read`，不能把“好嘞，我帮你读新闻”当作正文开始。

本次修正：

- `Protocol::SendMcpMessage()` 的消息原来以 `session_id` 开头，而 MQTT 的 MCP 分类只认
  `type` 开头，工具回包因此套用普通消息的内存限制。统一输出格式，并记录实际发送失败。
- 原限制在最大连续内部堆低于 20KB 时拒绝所有不小于 256B 的 MCP 消息。
  `mcp_payload_budget.h` 统一工具列表与发送器的预算：低内存时单包最多 2KB，
  另留 4KB 内部堆余量；列表再预留 256B 给外层包装。该余量仍需实机验证。
- 工具列表首页优先放新闻朗读和打开应用。正文按 JSON 转义后的 640B 预算分段，
  保留 UTF-8 边界，为标题、续读说明和外层 JSON 留出空间；不再回传 2800B 正文大段。
- 朗读请求只由真实、合法的新闻工具调用解除等待。等待期间不播云端抢答，
  12 秒没有工具调用重试一次，再失败显示提示；用户新指令或断开连接会取消等待。
  移除失效的六字 detect 分段路径，以及按“没有/无法/再见”等词判断新闻正文的逻辑。
- 用户触发的技能 invoke 与新闻正文请求不再等待语音空闲；后台心跳仍保持原让路策略。
- 补充新闻短名与日志中的“财新闻”完整短句匹配；处理“读。”的标点、拒绝朗读、
  明确选第几条，以及确认期间用户通过触摸改选文章的情况。

建议编译后的验证顺序：

1. 从启动开始保留串口日志，确认 `tools/list page_cap=... head=self.skill.news`，
   且没有相应的 `skip mqtt publish ... mcp=1` / `MCP response not sent`。
2. 唤醒后说“最新新闻”或“打开面板新闻”，确认列表及时出现，随后听到“要读吗”。
3. 回答“读”，确认先出现 `tools/call self.skill.news op=read`，再出现
   `news result op=read ok=1 bytes=...`，实际声音是文章内容。
4. 验证长文“继续读”、直接“朗读第二条”、详情页“朗读”按钮、“不要读”和关闭新闻。
   MCP 的 `continue` 返回后续段；设备等待本段 TTS 结束且音频播放队列排空后，
   自动请求云端调用 `continue`，不再只靠模型主动接力。
5. 若使用 WebSocket，重复打开、朗读、续读和中断流程。协议字段语义没有变化，
   MQTT 与 WebSocket 共用 `Protocol` 的 MCP 包装。

所提供日志中普通云端 TTS 和 Opus 播放成功，未出现分配失败或重启，
因此不能把无法朗读直接归因于设备性能不足。内部堆确实紧张，PSRAM 剩余较多也不能
替代 TLS/I2S 所需的内部连续堆。`i2s_channel_disable ... not been enabled` 后声卡仍成功
重新启用，这次未调整音频驱动；如修复后仍无声，需结合新日志继续验证这一层。

本次仅做非固件编译检查：修改范围的 clang-format 与 `git diff --check` 通过。
主机测试共 110 项，98 项通过、1 项因无 C++ 编译器跳过；11 项测试受 Windows
临时目录清理锁、缺少 Bash/C++ 编译器影响报错（含子用例共 12 个 error）。
未编译、烧录或实机验证。云端是否收到完整工具列表、TLS 的实际峰值内存、
长文续读及音频中断恢复仍需按上述步骤确认。

## 新闻自动续段与跟读滚动（2026-10-29）

- 旧版只在工具回包中要求模型“念完再调用 continue”，设备没有续段状态机。
  现在收到本段末尾的朗读句子、收到 TTS stop 且本地 PCM 输出队列排空后，才请求下一段。
  必须确实有 PCM 写入；只有回复文字、只有队列短暂空闲、重复 stop，都不会跳段。
  自动续段不再发 abort 或重置解码器，避免截断尾音及干扰已开始的工具调用。
- `continue` 的固定 8 秒限制改为真实完成状态，短段不用空等，长段也不能提前取走。
  中断、断线、切换文章和关闭卡片后停止自动接力；手动续读保留已确认的位置，
  暂停时最多回到当前尚未确认播完的句子，不再一律退回段首。
  分段仍限制 JSON 转义后 640 字节，优先在预算后半段的句号或逗号处分开。
- 删除每 1.2 秒按 14 字节/秒估算的滚动定时器。工具取段不会滚动；TTS
  `sentence_start` 到达后，按原文匹配定位当前句子，忽略标点、空白和英文大小写。
  云端等待期间不再空滚，已定位的旧字幕不倒退滚动；息屏不执行 LVGL 滚动。
  这属于句子级定位，当前协议没有逐字播放时间戳，不能保证每个字与声音精确同步。
  字幕有局部改写时可用后续明确原文片段重新定位；无法确认段尾时仍停止自动接力，
  不盲目跳过剩余正文。

主机回归测试 `scripts/tests/test_news_read_progress.py` 运行实际 C++ 进度类和应用收尾函数，
覆盖标点/标题匹配、短尾句、迟到/重复结束、未播音频、打断、换篇、断线和最后一段。
实机仍需用至少三段的长文检查连续朗读，分别验证 MQTT/UDP 与适用的 WebSocket 路径，
以及朗读中打断、重读、下一篇、关闭新闻和字号变化后的定位。未编译或烧录固件。

### `audio=1 complete=0` 后继续读回到开头

本次日志在 71228 ms 显示 `news playback drained: token=1 audio=1 complete=0 more=0`，
随后 79338 ms 确实调用 `op=continue`，80228 ms 却再次从“10月28日，合肥……”开始。
这是设备完成判定失败后，把 `s_speak_off` 回退到 `s_speak_off_prev` 导致的重读，
不能归因于模型没有调用 continue。日志没有包含原始 MCP 正文，无法确定最初失配的字符。

原匹配器要求每句匹配起点不得超过之前匹配终点。某一句有局部措辞差异，就可能使其后
所有正确字幕都被拒绝。现在允许用至少 8 个连续有效字符重新定位，并优先确认明确的
段尾匹配；仍保留 TTS stop、实际 PCM 输出、队列排空、当前文章和 token 校验。
正常完成后沿用已有的 `SendNewsReadRequest(false)` 自动请求下一段，不发 abort、不重置解码器。

续读位置区分两种情况：自然结束且音频排空，保存到已匹配句子的末尾；用户主动暂停，
保留尚未确认播完的当前句子边界。局部改写时的匹配锚点可能在句中，暂停不能直接跳过
锚点前的音频。新增 `news chunk` 和 `news cursor` 日志打印段起止、匹配终点、恢复位置和 more，
便于验证游标没有回零，也没有越过未确认的文本。

主机回归包含提供日志中的字幕、模拟原文局部措辞差异、连续三段自动请求两次 continue、
自然结束后的手动续读和暂停恢复。相同局部改写用例在修改前为 `complete=0 more=0`，
修改后为 `complete=1 more=1`。这验证的是匹配与接力逻辑，实机云端响应仍需编译后确认。
