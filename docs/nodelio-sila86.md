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
