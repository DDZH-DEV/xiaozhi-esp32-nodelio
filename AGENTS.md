# AGENTS.md

## Project

XiaoZhi is an ESP-IDF C/C++ voice-assistant firmware supporting many chips, boards, displays, audio devices, and network transports. A build selects exactly one board implementation.

Use ESP-IDF v6.1 when possible. The minimum supported SDK is ESP-IDF v6.0.1. IDF 5.x is not supported.

## Architecture

- `main/application.*`: main event loop, protocol lifecycle, and high-level behavior.
- `main/device_state_machine.*`: legal runtime state transitions.
- `main/boards/common/`: board interfaces and reusable hardware/network helpers.
- `main/boards/**/`: board-specific pins, initialization, and build variants.
- `main/audio/`: codecs, audio tasks, engines, wake words, and queues.
- `main/protocols/`: transport-neutral API plus WebSocket and MQTT/UDP.
- `main/display/` and `main/led/`: reusable UI implementations.
- `main/mcp_server.*`: common device-side MCP tools and dispatch.
- `main/Kconfig.projbuild`: board and feature configuration.
- `main/CMakeLists.txt`: source, board, locale, font, and asset selection.
- `scripts/build.py`: canonical board/variant build entry point.

Read the closest existing implementation before adding a new one. Prefer the narrowest owning layer; do not put board-specific behavior into core modules.

## Required Rules

- Preserve unrelated worktree changes and keep patches focused.
- A build must export exactly one board factory through `DECLARE_BOARD(...)`.
- Never alter an existing board's pins to support different hardware. Add a uniquely named board or release variant; board identity affects OTA compatibility.
- Core code depends on `Board` interfaces, never a concrete board class or board `config.h`.
- Treat camera, backlight, display, LED, battery, and similar capabilities as optional.
- Change runtime state through `Application::SetDeviceState()` and the state machine.
- Callbacks may run outside the main task. Schedule application mutations with `Application::Schedule()` or event bits.
- Do not block the main event loop or audio tasks. Avoid unbounded queues and repeated large allocations in audio paths.
- Keep shared message semantics in `Protocol`; verify both transports when changing its contract.
- Validate network input and preserve `cJSON` ownership. NVS keys are persistent API and require migration when changed.
- Guard target-specific features with Kconfig/component rules. Do not assume every target has PSRAM or S3/P4 resources.
- Do not manually edit generated/vendor output: `build/`, `releases/`, `managed_components/`, `components/`, `sdkconfig*`, `main/assets/lang_config.h`, or generated mmap headers.
- Format only touched C/C++ files with the repository `.clang-format`; avoid unrelated mass formatting.

## Boards and Configuration

Board selection is a coupled chain:

`config.json` -> `scripts/build.py` -> `main/Kconfig.projbuild` -> `main/CMakeLists.txt` -> board source and `config.h`.

When adding a board or variant, update every relevant link in that chain. Include a unique board identity, correct chip target, flash/partition settings, exactly one `DECLARE_BOARD`, and board documentation. Follow `docs/custom-board.md`.

## Commands

Source the intended ESP-IDF environment first:

```sh
source /path/to/esp-idf/export.sh
idf.py --version
```

```sh
# Discover exact board and variant names
python3 scripts/build.py --list-boards

# Canonical variant build
python3 scripts/build.py <board-directory> --name <variant-name>

# Host-side build tests
python3 -m unittest discover -s scripts/tests -v

# Format/check touched files
clang-format -i <files>
clang-format --dry-run -Werror <files>
```

The build script changes local `sdkconfig` and build state. Do not assume the build directory still represents a previous target.

## Validation

- Board-only change: build affected variants and smoke-test changed hardware.
- Core, common-board, audio, protocol, display, dependency, Kconfig, or CMake change: run host tests and build representative affected chip/network paths.
- Protocol changes: verify WebSocket and MQTT/UDP when shared behavior changes.
- Audio changes: verify capture, playback, wake/VAD, interruption, reconnect, and applicable AEC modes.
- UI/assets changes: verify applicable no-display/OLED/LVGL paths and partition size.
- Always report what was tested and what still needs physical hardware. A successful build is not hardware validation.

## Authoritative Documentation

- Overview and SDK policy: `README.md`
- Board guide: `docs/custom-board.md`
- Audio design: `main/audio/README.md`
- Code style: `docs/code_style.md`
- Protocols: `docs/websocket.md`, `docs/mqtt-udp.md`, `docs/mcp-protocol.md`
- CI matrix: `.github/workflows/build.yml`

Keep detailed or fast-changing information in those files, not here. Add a nested `AGENTS.md` only when a subsystem needs specialized instructions.

## NodeLio Panel（SILA-86 叠层）

> 本仓库（`D:\xz25`）同时承载 NodeLio 墙板固件（`main/nodelio/`、`main/boards/sila/sila-86/`）。

- **固件根**：本仓库（`main/nodelio/`、`main/boards/sila/sila-86/`）
- **计划/协议**：`D:\SITES\NodeLio\ESP32-IDF\PLAN-PLATFORM.md`、`D:\SITES\NodeLio\ESP32-IDF\docs\hw-constraints.md`
- **动态技能规划**：`D:\SITES\NodeLio\ESP32-IDF\docs\panel-skills.md`（PLAN 阶段 L）
- UI 令牌：`main/nodelio/ui_theme.h`；控件：`ui_widgets.*`；壳：`nodelio_overlay.cc`；**禁止**散落裸 hex
- **禁止** `idf.py flash`（代烧录）；本机 OOM 时用 `idf.py build -- -j2`
- **禁止**擅自恢复定时拉布局；勿把 `ESP32-IDF/base-design.jpg` 当唯一真理

### 省电优先（每次改固件默认按「省电优先」评审）

| 优先级 | 耗电大户 | 要求 |
|--------|----------|------|
| P0 | RGB 屏 + 背光 | 超时息屏关背光；触摸唤醒；禁止常亮调试背光留生产路径 |
| P1 | WiFi / HTTP / TLS | layout/snapshot **开机一次 + 手动刷新 + MQTT 推送**；未联网跳过 HTTP |
| P2 | LVGL / 轮询 | 息屏降刷新；忌短周期空转 |
| P3 | 音频 / AFE | 唤醒与 TLS/OTA **互斥**；会话结束释放 |

- **禁止**用 `deep sleep` 冒充本板「关机」。
- 新常驻任务须在 `hw-constraints.md` 写清亮屏/息屏电流影响（可定性）。

### 双电源方案区分

同批手板方案 A（TP4057）与 B（IP5306）勿混结论；CHRG 灯非 MCU GPIO。  
SILA-86 背光以板级 `config` 为准；勿盲目照抄旧叉 invert。

### 动态技能约束

- 第三方接口进面板：**走云技能清单**（`docs/panel-skills.md` / PLAN 阶段 L），**默认不发固件**。
- 固件只保留通用 invoke + 通用列表 UI；**勿为每个 API fork 页面**。
- 控家仍用 layout + `panel/command`；技能为平行通道。

### Flash / 功能落点（加功能先看）

- 分区：`partitions/v2/16m.csv`；产物 `build/`（`nodelio-build.bat`）。`xiaozhi.bin`→`ota_0`（~3.875MB）；`storage`→`0x800000`（1MB，layout/apps 等可写缓存）；`generated_assets.bin`→`0x900000`（7MB，WakeNet+字库）。整片：`flash_args`。
- 板级：`main/boards/sila/sila-86/`；控家 UI：`main/nodelio/`；离线口令：`offline_cmd*.c`。SILA 已在 `main/CMakeLists.txt` 裁掉 camera / 4G 板 / OLED / Emote / 未用 codec；勿无条件加回。`EspNetwork`（esp-ml307）服务 WiFi HTTP/MQTT，保留。完整表：`NodeLio/.codex/rules/esp32-idf-panel.md` §4.1。

### UI 视觉规范（曜夜微晶体系）

- 画布底色 `0x0c0f14`；关态卡片统一深石板 `0x161b24`，禁止各自着色；开态采用深环境微光底 + 高饱和 Icon Pod 胶囊底座 + 右上角药丸微标。
- 控件代码只读 `ui_theme.h`，严禁散落裸 hex；APP `wall-panel-layout.uvue` 须 1:1 骨架同构（Icon Pod + 右上微标 + 双行标题）。

### 触摸并发与命令队列（严禁阻塞 UI）

- **禁止在 LVGL 回调内同步阻塞调用 HTTP/MQTT**；必须先翻本地缓存 `refresh_home_tiles()` 乐观响应（0ms），再将命令投入后台 FreeRTOS 异步队列 `nl_cmd_async`；失败时回滚并 toast。
- 异步队列任务栈必须放在 PSRAM 且大小 >= 10KB（10240 bytes），严禁 4KB 局部栈引发 `stack overflow` 崩溃。

### 离线语音动态词表注入

- 引脚/设备重命名时，服务端下发拼音（`pinyinOn`/`pinyinOff`），`offline_cmd.c` 启动时动态注册 MultiNet 词表，严禁写死静态口令。

### 实机截图层级

- 播放器 / 技能卡 / 设置内页挂在 `lv_layer_top()`，与首页 `s_root` 同级。
- 截图必须拍 `lv_layer_top()`，禁止只拍 `s_root`（否则截不到播放器）。

### 音乐播放与息屏

- 播放中可息屏（不停播）；播放中禁止断网；息屏勿关功放。
- 设置页「播放器」：是否允许播放中息屏、暂停多久关播放器（NVS）。

### 语音导航 / 关技能弹窗

口令与 MCP 表：`NodeLio/ESP32-IDF/docs/xiaozhi-mcp.md`。

- 首页多屏：左右滑 / 「上一屏·下一屏」。有控家网格的首页不竖滚。
- 竖滚：设置、内页、连接、应用列表、启动台（「往上翻·往下翻」）。
- `goto_page home` **不关** `lv_layer_top()` 上的技能弹窗；关电台/新闻用 `self.skill.close`。
- **禁止**在聆听/说话/连接中走 `StopNotification()` 收尾（`ResetDecoder` + `EnableVoiceProcessing(true)`）。否则下一轮跳过 `SendStartListening`，云端不再出字。只在 `Notifying` 停 HTTP 播歌。

