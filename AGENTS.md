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

## NodeLio Panel（WALL_PANEL 多板叠层）

> 本仓库承载 NodeLio 墙板：`main/nodelio/` + `main/boards/sila/*`。  
> 完整规则：`D:\SITES\NodeLio\.codex\rules\esp32-idf-panel.md`（与 `.cursor/rules/esp32-idf-panel.mdc` 同步）。

| board_id | 目录 | 要点 |
|----------|------|------|
| `sila_86` | `main/boards/sila/sila-86/` | S3；480×480；ES8311+ES7210；板级 `nodelio-*.bat`；强制软件 AES |
| `sila_p4c5_62` | `main/boards/sila/sila-p4c5-62/` | P4+C5 Hosted；逻辑 1280×452；**ES8389 必留**；grid 默认 10×3 |

- **type_code**：`WALL_PANEL`；硬件用 `board_id`。**禁止**为换屏新开 type_code；**禁止**改 sila-86 引脚迁就 P4。
- **计划/协议**：`D:\SITES\NodeLio\ESP32-IDF\`（`hw-constraints.md`、`board-hal.md`、`panel-skills.md`）
- UI：`ui_theme.h` / `ui_widgets.*` / `nodelio_overlay.cc`；几何走 **`display_profile`**；禁止裸 hex
- **禁止** `idf.py flash`；OOM 时 `idf.py build -- -j2`；禁止擅自定时拉布局
- 一次只编一块板；OTA 按 `board_id` 分流

### 省电优先

| 优先级 | 耗电大户 | 要求 |
|--------|----------|------|
| P0 | 屏 + 背光 | 息屏关背光；触摸唤醒；禁常亮调试背光 |
| P1 | WiFi / Hosted / TLS | layout **开机+手动+MQTT**；未联网跳过 HTTP |
| P2 | LVGL | 息屏降刷新 |
| P3 | AFE | 与 TLS/OTA 互斥；会话结束释放 |

禁止 deep sleep 冒充关机。新常驻任务写 `hw-constraints.md`（P4 含 DSI+Hosted 并发）。

### 技能 / Flash / UI

- 技能走云清单；勿每 API fork 页面；控家 = layout + `panel/command`。
- 分区：`partitions/v2/16m.csv`。86：`main/boards/sila/sila-86/nodelio-*.bat`。62：`main/boards/sila/sila-p4c5-62/nodelio-*.bat`，**勿套用 86 裁掉 es8389**。
- 曜夜微晶 + Icon Pod；APP 与实机按 **display_profile** 1:1（480 方屏 vs 1280×452 侧轨）。

### 调度 / 口令 / 截图 / 播放 / 语音

- 禁 LVGL 回调同步 HTTP/MQTT；乐观更新 + 异步 Job（`nl_cmd_async` / JobScheduler）；PSRAM 栈 ≥10KB；资源成对释放。
- MultiNet 窗已下线；layout 拼音服务 **云 STT 旁路**，勿写死静态口令唯一源。
- 截图拍 `lv_layer_top()`。播放中可息屏、不断网、不息屏关功放。
- MCP/口令：`NodeLio/ESP32-IDF/docs/xiaozhi-mcp.md`。`goto_page home` 不关技能浮层；聆听/说话禁错误 `StopNotification`。
- 语音默认官方小智云，设置可切自建服。

### 踩坑硬约束

全文：`NodeLio/.codex/rules/esp32-idf-panel.md` §11；86 排障：`docs/nodelio-sila86.md`。

- **86** 关硬件 AES；**P4 勿盲抄**，以实测为准。PSRAM ≠ DMA。
- MCP 只认 `tools/call`；UDP 60ms 静音预热；新闻续段禁 abort；DSH 硬约束。

