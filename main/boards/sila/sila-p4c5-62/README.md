# SILA P4C5 6.2-inch NodeLio panel

用户在提交 `24352f0d` 修正后确认屏幕点亮、能够进入配网。尚未确认触摸、双麦、联网对话和并发稳定性。
本次在该提交上接入 **M3 最小产品闭环的源码**；本轮改动按用户约定未做 IDF 编译、未烧录，仍需实机验收。
现在启用 NodeLio 注册、首页、应用、设置和云 STT/MCP 控家，保留 P4 的小智激活流程。
S3 专用的硬件 AES、DRAM 占位、断网休眠及音频规避没有整体迁入 P4。

## 基线与证据

- 固件基线：`origin/nodelio-sila86`，`e3a9e0fc9ae64948e6545843d180282ced88dcc6`。
- 开发分支：`codex/sila-p4c5-62`。
- 用户 COM8 探测：P4 silicon v3.2、40MHz 晶振、USB Serial/JTAG；Secure Boot 和 Flash Encryption 均未启用。
- 卖家描述：ESP32-P4NRW32X、ESP32-C5-MINI、ES8389、AXP2101；用户有两块板，其中一块升级到 32MB Flash。
- 用户提供的 `SCH_P4X_2026-10-13.pdf`（11 页）：第 1 页总连接，第 2 页 MCU/外置 Flash，第 8 页音频。
  第 2 页标注 W25Q256JVEIQ / FLASH32M；这不是两块实物的容量检测结果。
- 原作者 `esp32-p4-c5-aibox`：显示/触摸参考 `mipi_lcd@4dc2455`，Hosted 参考 `origin/main@4cf7932`，
  旧音频参考 `origin/xiaozhi-p4c5@d32596f`。参考仓库未修改。
- AXS15260 的初始化寄存器数据保留 Apache-2.0 标识，移入本板源码，逐条检查发送错误。

**关键差异：新原理图 GPIO13 接 I2S_MCK → ES8389 MCLK，故本板 `use_mclk=true`。**
旧案例的无 MCLK 配置不适用于这张图。原理图未给出 6.2 寸模组完整料号/时序；AXS15260
及其时序仍是原作者案例对应的候选，需在用户购买的屏上验证。

## 引脚与所有权

| 功能 | P4 引脚/配置 | 说明 |
|---|---|---|
| LCD/TP reset | 26 | 共用；显示初始化复位一次，触摸禁止再拉低 |
| 背光 | 30，高亮/低灭 | PWM 20kHz，首个 UI 绘制后恢复保存亮度 |
| TP/PMU I²C | SDA28、SCL29，I2C0 | 板级拥有总线；TP 0x3B，PMU预期0x34 |
| TP interrupt | 27，下降沿 | 仅 IRQ 后 raw read；不空闲轮询 |
| PMU interrupt | 6 | 目前未注册 IRQ，也未写寄存器 |
| Codec I²C | SDA7、SCL8，I2C1 | ES8389 使用 esp_codec_dev 的地址常量 |
| I²S | MCLK13、BCLK12、WS10、DOUT9、DIN11 | 双工 24kHz/16bit；输入两麦，输出 mono 复制到 L/R |
| PA enable | 31 | 归 codec 管理；息屏不关闭正在播放的功放 |
| C5 SDIO | Slot1，CLK18、CMD19、D0..3=14..17 | 4bit、40MHz 起步，实际稳定速率待测 |
| C5 EN/BOOT | 54/53 | Hosted 独占 EN/reset；P4 BOOT GPIO设输入上拉，正常启动释放 strap |
| BOOT button | 35 | 短按切换对话；息屏短按只唤醒；长按进入现有 Wi-Fi 配网 |
| SD Slot0 | CLK43、CMD44、D0..3=39..42，PWRn51、CD52 | 首期不初始化、不占用/卸载 SDMMC host |

C5 模组图上 D0=IO8、D1=IO7、D2=IO14、D3=IO13、CLK=IO9、CMD=IO10、BOOT=IO28。
U14 引出其 UART/EN/BOOT。**COM8 探测的是 P4，不证明 C5 已有 Hosted 从机固件。**
首烧/恢复应使用匹配版本的 C5 从机工程和实际 U14 接线，不把 P4 合并镜像写给 C5，
也不自动更改 C5 的 strap/eFuse。P4 bring-up 固件不会自动烧写 C5。

AXP2101 总线只做地址 ACK 探测，不将 ACK 当作芯片 ID 验证，也不虚报电量。
图中 DCDC1=3.3V，ALDO2/3/4 为摄像头相关 C1V8/C1V5/C3V3；电池容量、充电限制与
实际贴装仍需确认。首期保持硬件电源默认配置，不初始化摄像头、陀螺仪和 TF 卡。

## 构建（在装有 IDF 的电脑执行）

优先 ESP-IDF `release/v6.1`，最低 v6.0.1；记录实际 IDF SHA。
本次按用户要求**未执行 IDF 编译、未烧录**。不要使用 SILA-86 的 `nodelio-build.bat`。

板级入口（本目录）：

```bat
nodelio-flash.bat              :: COM8 + 32m，build.py 后整片 flash+monitor
nodelio-flash.bat --nofresh    :: 只烧现有 build/
nodelio-monitor.bat            :: 仅串口（USB Serial/JTAG；重启后端口可能变）
```

或手动：

```sh
idf.py --version
python scripts/build.py --list-boards
python scripts/build.py sila/sila-p4c5-62 --name sila-p4c5-62-32m
```

仅在另一块板确认是至少 16MiB Flash 后，改用：

```sh
python scripts/build.py sila/sila-p4c5-62 --name sila-p4c5-62-16m
```

16MB 是当前工程默认 Flash 大小，所以 16m 变体不重复设置同值，但显式选择独立分区表。
32m 变体覆盖 Flash 大小及分区表。`type=sila-p4c5-62`，两者报告不同的 `name`。
控制台显式选择 USB Serial/JTAG，方便沿用当前探测到的原生 USB 端口查看日志；重启后端口号可能变化。
NodeLio 产品身份为 `board_id=sila_p4c5_62`、`type_code=WALL_PANEL`，注册携带显示尺寸及 `display_profile`。

| 变体 | OTA slot ×2 | storage | assets | 总边界 |
|---|---|---|---|---|
| 16m（容量待实测） | 各0x3f0000，3.9375MiB | 1MiB | 7MiB | 0x1000000 |
| 32m | 各0x600000，6MiB | 2MiB | 17.875MiB | 0x2000000 |

分区路径为 `partitions/nodelio-p4c5-{16,32}m.csv`。两种布局不可用 app-only OTA 相互转换；
将来切换要按对应构建产物的 `flash_args` 完整处理，并先保留需要的用户配置。
勿仅复制一个 `xiaozhi.bin` 就当成完整首烧包。

网络固件安装由 `FIRMWARE_OTA_ENABLED=n` 关闭，自动更新、直接安装入口和 MCP 升级工具均受控；
激活/语音服务器配置获取不受影响。待板型、分区、资源和 P4/C5 版本校验落实后才能重新启用。
它不等于已经实现可靠 OTA/回滚，也不修改安全 eFuse。

依赖继续使用仓库 `main/idf_component.yml`：LVGL ~9.5.0、esp_lvgl_port ~2.9.0、
esp_codec_dev ~1.6.2、esp-sr ==2.4.7、Hosted ^2.12.11、wifi_remote ^1.6.4。
这些是约束，不是本机解析到的精确版本。第一次成功构建时保存 `dependencies.lock`、生成配置、
IDF SHA、C5 固件版本/摘要及串口日志，后续按该组合重建。不要把 Hosted 3.x 的配置示例混进 2.x。

## 显示、音频与任务预算

- 物理452×1280，LVGL旋转90°，逻辑1280×452；触摸只交原始物理坐标，由 LVGL 统一旋转。
- 2-lane DSI / 1000Mbps，DPI 48MHz，RGB888；HBP/HFP/HSW=90/90/10，VBP/VFP/VSW=10/250/50。
  按总时序计算约47Hz；参考宏名称的“60Hz”不是本板实测帧率。
- DPI 两完整帧，LVGL 两个四分之一屏 buffer，均24bpp；仅这些约4.14MiB，另需 PPA/对齐/资源空间。
  PPA 开启，`sw_rotate=true`，`direct_mode/full_refresh/avoid_tearing=false`。
- 继承 P4 默认 PSRAM 200MHz/XIP，并开启启动内存检查。这是待实测的起点；若不稳定，
  使用新的板级配置进行单变量验证，不通过关闭 WDT 掩盖问题。生产 WDT 保留，超时20s。
- `sila_touch`：4KiB 内部栈、优先级4、IRQ通知唤醒；I²C超时50ms，错误释放指针、吞掉不确定手势。
  不拥有总线，不做网络/NVS工作；LVGL缓存更新与读事件在同一锁内。
- `sila_idle`：每秒 esp_timer 回调，仅合并排入最多一个待处理检查到 Application，无独立线程。
  默认60秒无触摸/按键或对话状态活动后息屏；LVGL刷新定时器30→200ms；触摸/BOOT/对话唤醒后恢复。
  空闲态后台重连的性能请求不会点亮屏幕。
  息屏不停止 DSI、Hosted、音频和唤醒 AFE，**不是关机**。亮/灭屏电流与栈水位待实测。
- 双麦输入格式是 `MM`，没有虚构 `R` 回声参考。ES8389 的左右 DAC 分别接功放，原理图没有
  证明“麦克风第二通道就是扬声器回采”；设备端 AEC 不启用，播中唤醒/全双工效果尚未验收。
- 双扬声器扩展用 codec 初始化时分配的240帧缓存分块，避免每个音频包新建大 vector。

## 下一台电脑的验证顺序

1. 对两块板分别记录 Flash 检测结果、PCB丝印、屏模组标签；确认本图适用当前实物。
2. 构建对应变体；检查没有 unknown Kconfig symbol、目标为 esp32p4、最小硅版本为3.1，
   并确认 D1..3 配置为带 `_SLOT_1` 后缀的15/16/17。SDK 的 REV_MIN_301 覆盖已探测的 v3.2。
3. 首次启动记录实际 Flash/PSRAM、PMU ACK，以及 `before peripherals` / `display initialized` /
   `first UI` / `before Hosted/Wi-Fi` 的内存日志。用 GPIO35 在无触摸时控制基础流程。
4. 检查色条/文字/四角、拖动、抬手、快速连续点击、I²C失败恢复、息屏首触摸不透传。
   IRQ 丢失/长期按住的行为必须实测，当前没有猜测触摸内部寄存器来做轮询恢复。
5. 确认 C5 从机版本，测试配网、错误密码、路由重启、DNS/TLS失败与重连；无从机先解决 C5，
   不据此判定 LCD 或产品后台故障。5GHz/蓝牙功能不能只凭卖家介绍标为软件已支持。
6. 两个麦克风分别敲击/录音验证通道；检查左右扬声器、音量、24kHz时钟、重新启用录放音、
   唤醒、打断。随后做最大背光 + DSI刷新 + Hosted/TLS + 双工音频并发验证。
7. 验证本轮 M3 原型：绑定服务端、创建布局、触摸控家、语音控家、打开技能、播放/停止、断线重连。
   已保存 6×6 布局保持原坐标；新建 P4 布局默认 10×3。M4/M5/M6 的调度迁移、可靠升级及长稳验收仍未完成。

本机验证记录见开发计划；宿主测试和源码检查不能代替目标编译或物理硬件验证。


## 本轮使用入口与跨端配套

- 左侧导航：**首页 / 应用 / 设置**。技能卡放右侧，播放器放底部。设置卡使用四列横屏排列。
- **设置 → 硬件自检**：拖动触摸区观察逻辑坐标；分别点 MIC 1、MIC 2，约 5 秒录音后自动本机回放。
  原始峰值和采集帧数用于诊断，不代表声压校准或 AEC 通过。录放音复用有界 Opus 队列，不上传测试录音。
  仅待机/配网时允许开始；对话、播放、激活时拒绝；离开页面/停止/联网切换会取消。总时限 15 秒。
  两路回放都以 mono 复制给双扬声器；分别检查每只扬声器有声，本测试不区分左右 DAC 独立声道。
- **设置 → 显示与声音**：亮度、音量；**省电**：空闲/退下息屏，P4 不开放断网休眠。
- **设置 → 网络与连接**：设置 NodeLio 服务地址与绑定信息。产品 HTTP/MQTT 沿用现有协议；不定时重拉布局。
- P4 注册 `display_profile={id:"wide_1280x452",cols:10,rows:3,nav_width:96}`；`display={w:1280,h:452,touch:...}`。
  `touch` 按驱动初始化结果报告；失败时 BOOT 仍可用，心跳不报 `input.touch`。
- 同时更新产品仓：SERVER `0.4.64` 按设备配置选择**新建**布局；APP `1.0.34 / 10034` 使用对应网格、侧轨预览和可用控件尺寸。
  旧布局不会自动改为 10×3；要体验新网格，应使用新建布局或显式迁移，不能直接改网格后让旧坐标越界。
  老 SERVER 仍会创建 6×6，固件兼容显示；部署配套 SERVER/APP 后再验证完整横屏编排。
- 新增页面仅一个 250ms LVGL 定时器，删除页面时释放；没有新增网络任务。音频测试不创建第二个 I²S 读任务。

本机检查：宿主套件 112 项，0 断言失败、12 条环境错误、3 跳过（临时目录占用、缺 bash/C++）；
APP 的 Vue 模板、TypeScript 语法和 profile 分支检查通过。未做 IDF/Go/uni-app 目标构建或实机性能测试。

## API 复核来源

- [IDF 6.1 DSI API](https://github.com/espressif/esp-idf/blob/release/v6.1/components/esp_lcd/dsi/include/esp_lcd_mipi_dsi.h)
- [IDF P4 硅版本配置](https://github.com/espressif/esp-idf/blob/release/v6.1/components/esp_hw_support/port/esp32p4/Kconfig.hw_support)
- [Hosted 2.x Kconfig](https://github.com/espressif/esp-hosted-mcu/blob/release/v2.x/Kconfig)
- [esp_lvgl_port 2.9.0](https://components.espressif.com/components/espressif/esp_lvgl_port/versions/2.9.0/readme)
