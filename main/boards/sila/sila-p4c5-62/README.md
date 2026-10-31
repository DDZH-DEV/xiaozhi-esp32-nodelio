# SILA P4C5 6.2-inch NodeLio panel

用户在提交 `24352f0d` 修正后确认屏幕点亮、能够进入配网。2026-10-31用户确认基础触摸与唤醒正常；最新日志越过Hosted RX崩溃点，有云端回复，但唤醒后说话无识别。完整对话及并发稳定性待复测。
本次在该提交上接入 **M3 最小产品闭环的源码**；本轮改动按用户约定未做 IDF 编译、未烧录，仍需实机验收。
现在启用 NodeLio 注册、首页、应用、设置和云 STT/MCP 控家，保留 P4 的小智激活流程。
S3 专用的硬件 AES、DRAM 占位、断网休眠及音频规避没有整体迁入 P4。

## 基线与证据

- 固件基线：`origin/nodelio-sila86`，`e3a9e0fc9ae64948e6545843d180282ced88dcc6`。
- 产品代码已并入 `nodelio-sila86`（`eaa0c094`），后续按用户指定的产品主分支维护。
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
U14 引出其 UART/EN/BOOT。**COM8 的 ROM 探测仅识别 P4；本次应用日志已确认 C5 运行 Hosted 2.12.0。**
首烧/恢复应使用匹配版本的 C5 从机工程和实际 U14 接线，不把 P4 合并镜像写给 C5，
也不自动更改 C5 的 strap/eFuse。P4 bring-up 固件不会自动烧写 C5。

本目录 `nodelio-flash-c5.bat`：从 `managed_components/espressif__esp_hosted/slave`
编 **esp32c5** Hosted 从机到独立目录 `build-c5-hosted-slave/`，经 **U14 UART** 烧录。
它用于 UART 首刷/恢复；当前有可运行的 C5 固件，不需要为了本次 RX 内存错误先做 UART 首刷。
主从版本差异需要单独验证/升级，但不能把版本警告直接等同于 DMA 内存分配失败。

```bat
nodelio-flash-c5.bat COM9
nodelio-flash-c5.bat COM9 --monitor
```

C5 SDIO 固定脚：CLK9、CMD10、D0=8、D1=7、D2=14、D3=13（与模组图一致）。

### 本次启动重启：Hosted 接收缓冲区

用户日志：`Identified slave [esp32c5]`、`STREAMING MODE`、C5 `2.12.0`，随后
`failed to allocate buffer for slot 1 in double_buf`，最后在
`bus_sdio_set_rx_buffer_size()` 的 `ESP_ERROR_CHECK` 处 abort。这证明 SDIO 已建立，
直接崩溃点是 **P4 接收缓冲区内存分配失败**。I²S 双工切换提示不是这次 abort 的来源。

网络启动前 INTERNAL free=100263、largest=55296；这不是 DMA 能力过滤后的可分配数值。
Hosted 默认从内部 DMA 堆分配传输池和 streaming RX 双缓冲，显示、音频已经占用了内部内存。
如果 C5 TX 队列为20，两个 streaming 缓冲本身就需要 `2 × 20 × 1536 = 61440` 字节，
另有传输池、任务栈等；日志中的 Host Rx/Tx=20 不能代替 C5 TX 队列的实测值。
普通 PSRAM 空闲24MB也不会自动被默认的内部 DMA 分配器使用。

本板五个变体统一设置：

```text
CONFIG_ESP_HOSTED_MEMPOOL_PREFER_SPIRAM=y
CONFIG_CACHE_L2_CACHE_LINE_128B=n
CONFIG_CACHE_L2_CACHE_LINE_64B=y
CONFIG_ESP_HOSTED_SDIO_OPTIMIZATION_RX_STREAMING_MODE=y
CONFIG_ESP_HOSTED_SDIO_RX_DOUBLE_BUFFERING=y
```

使用官方 DMA PSRAM 分配路径；保留 streaming 双缓冲和40MHz总线。64字节缓存行与本板
LVGL/PPA的64字节对齐一致。未改第三方驱动、未移除 assert/WDT，也未缩小从机发包上限。
板级编译检查会拒绝缺少上述 PSRAM/缓存行设置的旧 sdkconfig。
启动日志增加 DMA internal/PSRAM 的 free/largest，以及实际 Hosted 版本。

仅本板固定 `esp_hosted ==2.13.1`（官方预分配实现）；其他板保持原有版本范围。
日志虽自报Host 2.13.0，其调用栈已含官方2.13.1才发布的预分配函数；无法仅凭日志确认
编译电脑是否混用了组件、补丁或缓存。请重新用板级 build.py 构建，核对 `dependencies.lock`
中的 Hosted 版本和构建日志，不要用 `--nofresh` 重烧旧镜像，也不要手改 managed_components。

验收先看启动能到 `TRANSPORT_TX_ACTIVE` / `RPC_WRAP: Coprocessor Boot-up`，确认配网、DHCP、
TLS及断线重连，再测试 UI、双麦、播放并发。C5仍为2.12.0时版本警告会保留，不能宣称主从已匹配。
若仍分配失败，保留从启动开始的完整日志，尤其新加的 DMA 堆数值；不要靠屏蔽错误继续收包。
GPIO30的LEDC占用提示、MAC读取错误也需后续验证，当前未据此更换引脚。

### 原作者所说的 SDIO 烧录

本地参考仓 `origin/main@4cf7932` 的 README / `main.c` / `main/c5_sdio_flasher.c` 区分两条路径：

- **ROM 首刷**：`esp-serial-flasher`，需要控制BOOT/EN并进入C5下载模式；参考工程默认关闭。
  作者文档记录其样板 GPIO25/MTDI strap 导致 ROM SDIO 下载不通，这是参考板的实测记录，
  不能仅凭 PCB 名称就判定用户这块 v3.2 也有相同硬件问题。
- **已有 Hosted 的从机 OTA**：P4 经现有 SDIO RPC 给 C5 写入新应用，适用于当前已能响应的C5；
  不需要六孔UART、不进入ROM。原作者示例的 main 默认只释放BOOT并启动Wi-Fi/SD卡，
  没有自动执行这条OTA。产品固件同样不会开机自动更新C5。

后续需要对齐 C5 时，使用与本板Host相同的2.13.1源码构建 `slave/` 的 **esp32c5** 应用，
确认从机当前分区存在足够大的OTA槽，再使用官方 `examples/host_performs_slave_ota` 的
`begin → write → end → activate` 流程。OTA输入是 `network_adapter.bin`，不是P4镜像，
也不是包含bootloader/分区表的合并镜像；OTA不会迁移已有C5分区表。
维护Host需配置本页的Slot1引脚和BOOT53释放，并保留本板P4 v3.x配置；不要直接烧默认C6示例。
示例中Partition/LittleFS方案可离线传输，不依赖C5已连路由器。
这一步先于长期主从兼容验收，但不是修复当前内部DMA分配失败的替代方案。

来源：[Hosted 2.13.1分配器](https://github.com/espressif/esp-hosted-mcu/blob/v2.13.1/host/port/esp/freertos/src/port_esp_hosted_host_os.c)、
[SDIO接收实现](https://github.com/espressif/esp-hosted-mcu/blob/v2.13.1/host/drivers/transport/sdio/sdio_drv.c)、
[官方从机OTA示例](https://github.com/espressif/esp-hosted-mcu/tree/v2.13.1/examples/host_performs_slave_ota)。

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

本板在 `main/idf_component.yml` 固定 LVGL **9.5.0**、esp_lvgl_port **2.9.0**，
通过板型 Kconfig 条件选择；其它板仍使用原来的 ~9.5.0 / ~2.9.0。
esp_codec_dev ~1.6.2、esp-sr ==2.4.7、Hosted ^2.12.11、wifi_remote ^1.6.4。
这些是约束，不是本机解析到的精确版本。第一次成功构建时保存 `dependencies.lock`、生成配置、
IDF SHA、C5 固件版本/摘要及串口日志，后续按该组合重建。不要把 Hosted 3.x 的配置示例混进 2.x。

## 渲染对照实验（源码已接入，未做目标构建/实测）

在用户提交 `07bb8989` 上增加以下 32MB 实验构建，均保持 RGB888、90° port PPA 旋转、
PARTIAL 模式和原分区。普通 16m/32m 构建不开填充实验或周期性能日志。

| 构建名后缀（前缀均为 `sila-p4c5-62-32m-`） | LVGL 绘制 | 每个 LVGL 缓冲 | 性能日志 |
|---|---|---|---|
| `render-sw` | 软件 | 四分之一屏 | 开 |
| `render-ppa` | 板级 PPA 填充 + 软件 | 四分之一屏 | 开 |
| `render-ppa-full` | 板级 PPA 填充 + 软件 | 完整屏 | 开 |

```sh
python scripts/build.py sila/sila-p4c5-62 --name sila-p4c5-62-32m-render-sw
# 留存上述产物与日志后，再构建下一组；每次 build.py 会更换当前构建配置。
python scripts/build.py sila/sila-p4c5-62 --name sila-p4c5-62-32m-render-ppa
python scripts/build.py sila/sila-p4c5-62 --name sila-p4c5-62-32m-render-ppa-full
```

实验仅用于已确认容量的32MB板。现有 `nodelio-flash.bat` 默认会重新构建普通变体，
不用于选择这些实验；烧录时使用相应构建保存的产物。所有变体仍禁用网络固件更新。

- 启动打印实际 LVGL 版本、像素缓冲大小、旋转后端、板级填充是否成功注册及对齐配置。
- `NODELIO_P4_RENDER_STATS` 使用 LVGL refresh 事件汇总有绘制的刷新耗时，每隔至少5秒在下一次
  有绘制的刷新结束时输出平均/最大值和堆内存；无额外定时器、任务或强制重绘。
  `wall_us` 包含 LVGL 本次刷新内等待，不代表 CPU 利用率、完整屏幕扫描时间或触摸端到端延迟。
- `PPA fill total` 是启动以来成功的填充次数；`fallback` 是已接管任务转回软件的次数，
  不包括一开始就由软件处理的圆角/渐变/图片。计数持续为0时不得声称绘制已得到加速。
- 在同一布局、亮度、网络和音频条件下依次比较 sw→ppa、ppa→ppa-full；测试首页切换、滚动、
  技能卡+播放器、双麦自检、联网语音、息屏唤醒，记录错色/花屏、音频欠载和内存最低值。
  加速能否默认启用由结果决定，目前不宣称性能提升。

### 为何没有直接开启 LVGL 9.5 自带 PPA / 升到9.6

审查官方 9.5.0 `lv_draw_ppa_buf.c` 发现其同一缓存回调在 DMA 前后均执行 C2M，
不具备此处需要的明确 DMA→CPU 无效化步骤。因此 `CONFIG_LV_USE_PPA` 保持关闭，
实验使用本板 `ppa_fill.cc` 的独立 draw unit（`NODELIO_P4_PPA_FILL`），不修改 vendor 文件。
它仅接管至少4096像素、无圆角/渐变、完全不透明的 RGB888 填充；缓冲必须在 PSRAM、
地址与长度均按缓存行对齐、stride 紧密。DMA 前 C2M、结束后 M2C，单软件绘制单元串行执行。
其它任务留给软件；PPA 初始化失败保持软件路径，运行错误会停用加速并重绘当前区域。
不注册额外 SRM/混合客户端，输出旋转仍由 port 独占自己的 SRM 客户端。
该适配明确依赖固定的9.5内部 draw-unit API，升级版本时必须重新审查，不能悄悄套用。

9.6.0~1 已发布，但 `78/xiaozhi-fonts 2.0.0` 的 manifest 限定 `lvgl/lvgl: ~9.5.0`，
`cbin_font.c` 又按 `sizeof(lv_font_t)` / `sizeof(lv_font_fmt_txt_dsc_t)` 读取二进制资源。
不强行覆盖这条依赖。下一轮9.6迁移需同时处理字体提供方、二进制格式/重生成/加载校验，
再处理公开头文件、内存配置与截图等接口；不能只改 LVGL 版本号。本轮未升级到9.6。

## 显示、音频与任务预算

- 物理452×1280，LVGL旋转90°，逻辑1280×452；触摸只交原始物理坐标，由 LVGL 统一旋转。
- 2-lane DSI / 1000Mbps，DPI 48MHz，RGB888；HBP/HFP/HSW=90/90/10，VBP/VFP/VSW=10/250/50。
  按总时序计算约47Hz；参考宏名称的“60Hz”不是本板实测帧率。
- DPI 两完整帧，LVGL 两个四分之一屏 buffer，均24bpp；仅这些约4.14MiB，另需 PPA/对齐/资源空间。
  开启的是 port 输出旋转 PPA（`CONFIG_LVGL_PORT_ENABLE_PPA=y`），`sw_rotate=true`，
  `direct_mode/full_refresh/avoid_tearing=false`；这不表示启用了 LVGL PPA 绘制单元。
  普通变体明确关闭 LVGL 自带 PPA 绘制；板级填充实验见上节。
  计入 port 的一份旋转缓冲后，普通配置像素内存约4.55MiB，完整缓冲实验约8.28MiB；
  尚不含其它资源、对齐和运行时分配。完整缓冲实验仍不是 `full_refresh` 或 `direct_mode`。
- 继承 P4 默认 PSRAM 200MHz/XIP，并开启启动内存检查。这是待实测的起点；若不稳定，
  使用新的板级配置进行单变量验证，不通过关闭 WDT 掩盖问题。生产 WDT 保留，超时20s。
- `sila_touch`：4KiB 内部栈、优先级4、IRQ通知唤醒；I²C超时50ms，错误释放指针、吞掉不确定手势。
  不拥有总线，不做网络/NVS工作；LVGL缓存更新与读事件在同一锁内。
- `sila_idle`：每秒 esp_timer 回调，仅合并排入最多一个待处理检查到 Application，无独立线程。
  默认60秒无触摸/按键或对话状态活动后息屏；LVGL刷新定时器30→200ms；触摸/BOOT/对话唤醒后恢复。
  空闲态后台重连的性能请求不会点亮屏幕。
  息屏不停止 DSI、Hosted、音频和唤醒 AFE，**不是关机**。亮/灭屏电流与栈水位待实测。
- ES8389采集声明已从错误的`MM`纠正为`MR`：当前`esp_codec_dev 1.6.2`以`no_dac_ref=false`
  输出ADCL+内部DACR参考，日志`Set internal reference signal`与此一致。第二槽不是第二麦，
  独立双麦路由还未实现/验收；设备端全双工AEC仍不默认启用，播中唤醒效果待测。
- 该版ES8389输出mute回调操作ADC寄存器0x20。项目codec适配层直接读改写DAC 0x40低两位，
  保留数据格式和共享TX时钟，避免扬声器静音误关采集；读取失败返回0而非伪报整帧成功。
  核对依据：[官方1.6.2源码](https://github.com/espressif/esp-adf/blob/af7b72fb2b73d4f513d3cede01c13518ab216735/components/esp_codec_dev/device/es8389/es8389.c)。
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
6. 当前MR配置先验证MIC1录音/回放；第二槽是内部DAC参考，不能当第二麦。检查左右扬声器、音量、24kHz时钟、重新启用录放音、
   唤醒、打断。随后做最大背光 + DSI刷新 + Hosted/TLS + 双工音频并发验证。
7. 验证本轮 M3 原型：绑定服务端、创建布局、触摸控家、语音控家、打开技能、播放/停止、断线重连。
   已保存 6×6 布局保持原坐标；新建 P4 布局默认 10×3。M4/M5/M6 的调度迁移、可靠升级及长稳验收仍未完成。

本机验证记录见开发计划；宿主测试和源码检查不能代替目标编译或物理硬件验证。

### 亮度与屏幕端连接设置回归（2026-10-31，待实机）

- 亮度预览与音量写入已分离；背光渐变仅由定时器写硬件，状态访问同步，抬手时即使
  已到目标亮度也保存。保存失败返回错误，不再因亮度 NVS 写入错误直接 abort。
- 偏好设置只保存偏好字段，注册结果只合并当前服务器/账号的凭据，避免旧快照覆盖连接。
  屏幕“网络与连接”每次打开从 RAM 缓存回填，保存后回读核对再安排重启；单改账号也清理旧 MQTT 凭据。
- 编译烧录后测试：亮度快速往返拖动、抬手后重启、息屏唤醒；分别测试只改 IP、只改账号、
  同时修改，重启后重新打开表单核对，并确认后台重新注册。Wi-Fi 密码输入框按原设计不回显。
  再调一次亮度并重启，确认刚保存的服务器/账号仍保留。若仍重启，保留 panic/backtrace 串口日志。
- 本轮宿主套件 112 项：98 通过、3 跳过、11 环境错误（Windows 临时目录占用、缺 Bash/C++）。
  未进行 ESP-IDF 目标编译、烧录或实机验收，尚不能确认用户遇到的亮度重启根因。


## 本轮使用入口与跨端配套

- 左侧导航：**首页 / 应用 / 设置**。技能卡放右侧，播放器放底部。设置卡使用四列横屏排列。
- **设置 → 硬件自检**：拖动触摸区观察逻辑坐标；按实际配置显示麦克风通道；当前MR只显示MIC1，约5秒录音后自动本机回放。
  原始峰值和采集帧数用于诊断，不代表声压校准或 AEC 通过。录放音复用有界 Opus 队列，不上传测试录音。
  仅待机/配网时允许开始；对话、播放、激活时拒绝；离开页面/停止/联网切换会取消。总时限 15 秒。
  录音回放以mono复制给双扬声器；分别检查每只扬声器有声，本测试不区分左右 DAC 独立声道。
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
