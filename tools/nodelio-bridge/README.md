# NodeLio Bridge

Go 实现的跨平台工作台连接桥。单个程序内含配置网页、DSH / OpenAI / WorkBuddy 连接器和 NodeLio 出站 WebSocket。用户分别启用需要的连接器。

这是 **0.2.0 联调预览版**：旧 `../dsh-voice-bridge` 保留，不自动替换、停止旧桥或注册启动项。正式发布的代码签名、公证和真实账号/墙板联调尚需完成。

## 支持范围

| 项目 | 本版本 |
|---|---|
| 构建目标 | Windows / macOS / Linux，各 amd64、arm64；CGO_ENABLED=0 |
| DSH | 本机 endpoint 自动发现、会话列表、详情、投递、续聊、按当前 notice 校验审批、zstd 日志 |
| OpenAI | Responses API、异步独立会话、previous_response_id 续聊；不访问 ChatGPT 历史、不执行本机工具 |
| WorkBuddy 本地 | 已授权 Token、在线检测、消息发送、最近消息历史；它是一条本地助理消息流，不是独立会话集合 |
| WorkBuddy 云端 | 创建任务、ACP SSE/JSON-RPC、续聊、流式文本、带原始选项的工具审批；以 prompt response 判断本轮结束 |
| 配置 | 三个独立开关、默认平台、配置热应用；监听地址改变需重启 |
| 登录自启 | Windows 用户计划任务、macOS LaunchAgent、Linux systemd 用户服务；配置页显式选择 |
| Host | 通过 `/ws/workbench-bridge` 使用 register/invoke/fetch/hint；市场包逐字节传输并校验哈希 |

第三方桌面应用是否支持某个系统/架构由其发行方决定。Linux ARM64 可使用云 API，不能据此认定同机 DSH 可用。

## 启动

构建环境需要 Go 1.24 或更高版本。终端位于本目录：

```sh
go build -o nodelio-bridge ./cmd/nodelio-bridge
./nodelio-bridge serve --open
```

Windows 源码构建建议使用 `go build -o nodelio-bridge.exe ./cmd/nodelio-bridge`。发行打包的 Windows exe 使用 GUI 子系统，不弹黑窗口；运行日志在数据目录。

直接双击 Windows exe 或 macOS `.app` 会启动并打开浏览器配置页。Linux `./nodelio-bridge serve` 可在无桌面环境运行，`--open` 按需调用浏览器。

程序默认只监听 **127.0.0.1:8791**。配置页首次访问带本机管理令牌，浏览器将令牌移入 sessionStorage 并移除地址栏 fragment。后续可重新运行程序，或使用 `nodelio-bridge config` 打开当前配置页。

配置步骤：

1. 填入 NodeLio Host 的 `/ws/workbench-bridge` 地址和桥凭证（SERVER 0.4.82 起支持；原 `/ws/dsh-bridge` 地址继续兼容）。
2. 打开需要的工作台开关，填写各自配置，保存并检查连接。
3. DSH 新建任务需填写有效的默认工作区及明确的权限预设；不会自动回退到更高权限。
4. OpenAI 填写账号可用的模型 ID 与 API Key。
5. WorkBuddy 填写开放平台已授权的 Access Token，选择 local/cloud。**尚未提供一键 OAuth 登录和令牌自动刷新**；过期后在此更新。应用 client_secret 不应嵌入发行包。
6. 使用“试用工作台”验证派活和续聊，再在 NodeLio APP 添加“电脑工作台”市场源（旧“家里 DSH”地址仍兼容），安装所需平台的主应用。桥及平台不可用时先完成配置，再刷新市场安装。
7. 程序放入固定目录后，在配置页选择“登录电脑后自动启动”。

应用和令牌的具体权限由平台决定；文档：https://developers.openai.com/api/docs/guides/conversation-state 、https://open.workbuddy.cn/docs/openapi 。

## 平台安装

`go run ./cmd/package` 生成六个便携包及 SHA256SUMS 到 `dist/`，不上传、不安装。

- Windows：解压后运行 `install.ps1`，复制到当前用户的 `%LOCALAPPDATA%/Programs/NodeLioBridge` 并创建开始菜单快捷方式。不会注册自启，须在页面开启。
- macOS：将 `.app` 放入固定的 Applications 目录，或运行包内 `install.sh`。开发包未签名/公证；面向用户发行前必须完成签名、公证，不能把交叉编译成功视为可直接公开分发。
- Linux：运行 `install.sh` 安装到 `~/.local/bin`。无 systemd 时登录自启接口明确返回不支持，可用发行版进程管理器执行 `serve`。
- ARM32 不在首版矩阵中。系统最低版本、Linux 发行版兼容性仍需在目标机器验收。

登录自启只管理桥，不打开第三方工作台。macOS LaunchAgent 在下次登录加载；关闭自启只移除后续登录注册，不终止当前程序。Linux 用户服务未开启 linger，默认随用户会话运行。

尚未实现 Windows Service / LaunchDaemon / systemd 系统服务的一键安装。需要未登录常驻时应先明确桌面代理与纯 API 服务的部署边界。

CLI：

```sh
nodelio-bridge serve --data-dir /absolute/private/directory
nodelio-bridge config
nodelio-bridge autostart-enable
nodelio-bridge autostart-disable
nodelio-bridge autostart-status
nodelio-bridge version
```

自启动管理命令与程序运行必须使用同一 `--data-dir`。配置页可以退出桥；升级前先退出，不按端口或进程名杀其他程序。

## 数据与请求语义

默认数据目录使用系统用户配置目录下的 `NodeLioBridge`：Windows `%APPDATA%`、macOS `~/Library/Application Support`、Linux `$XDG_CONFIG_HOME` 或 `~/.config`。

- `settings.json`：一个原子写入的文档，内部区分配置与凭证；API 只返回配置和凭证是否设置。凭证目前为文件存储，**没有宣称使用系统钥匙串加密**。
- `sessions.json`：云会话映射、当前状态和有限结果文本。最多 1000 条记录，达到上限时拒绝新建，需停机备份并整理历史。
- `requests.json`：携带 request_id 的变更请求回执；最多 4096 条，完成回执至少保留七天；未确定请求不自动清除或重发。
- `config-url.txt`：包含本机管理令牌，不能公开共享。
- `bridge.log`：运行日志；启动时按 4 MiB 阈值保留上一份。不会记录 API Key、Host Token 或完整指令。

Unix 数据目录权限 0700、文件 0600；Windows 对数据目录设置当前用户 ACL。凭证不进入市场包、墙板或日志。管理页防跨站请求和 DNS rebinding，接口要求 Bearer 管理令牌。

会话绑定平台；指定会话与平台冲突会拒绝，不静默转交其他平台。OpenAI / WorkBuddy 云端会话还绑定当前端点及凭证指纹；更换令牌会建立新的身份范围，旧记录保留但不会混入新身份。暂未提供跨令牌的账号迁移。

长任务立即返回本地任务 ID，实际请求在有界后台任务池执行。断线、超时和进程重启后的未确定请求标记为 unknown，不重新派活。云端任务最大执行等待窗口 10 分钟；超出后需在上游核实，不能把桥停止等待显示成远端任务已停止。

配置开关影响后续操作；已经提交的任务仍允许完成并保存结果。WorkBuddy 有待确认任务时不要切换连接模式或账号。通知队列跨 WS 重连保留，容量 64；进程退出后不持久重发通知，结果以会话存储为准。

## 从旧 DSH 桥迁移与回退

保留旧目录和配置。在新桥未运行、全部连接器关闭时显式导入：

```sh
nodelio-bridge import-legacy --legacy-config /path/to/dsh-voice-bridge/bridge-config.json
```

导入 Host WS、桥凭证、面板 ID、DSH 模型/权限/Agent 预设；不覆盖旧文件，不继承旧 LAN 暴露、自动拉起桌面、热重载或外部 TTS 依赖。端点和数据目录默认自动发现。

当前 Host 的 `DshBridgeHub` 按 owner 保留最近一条桥连接，**同账号不要让旧桥和新桥同时连接 Host**。多电脑按 bridge_id 路由尚未实现。配套 SERVER 0.4.81、APP 1.0.39 已接入连接状态与新版市场；完整使用还需部署配套墙板源码。

市场继续沿用 `com.nodelio.dsh` 和 `nodelio-dsh://` 来源，市场展示 dsh/openai/workbuddy/workbench 四个 `.status` 主应用，包版本 2.1.0。每个主应用内完成查看、派活、续聊与确认；不必单独安装 task/answer。旧 helper 调用继续兼容。新技能声明 outbound_bridge，不要求 Host 访问本机 LAN。`workbench.*` 根据显式 provider、已绑定会话或默认平台路由。

迁移预览版与旧桥尚有以下差异，不能直接视为等价替换：

- DSH 没有指定会话时创建新会话；旧桥可复用空闲会话。
- DSH 复杂多题提问提示回桌面回答；二元审批按实时 notice/action 校验。暂未搬入旧桥的逐题答案草稿 UI。
- DSH 暂无后台状态监视通知，需手动刷新；OpenAI/WorkBuddy 云任务由执行通道推送完成/审批通知。
- 暂不提供旧版 Edge/SAPI 本地 TTS，Envelope 保留文字 spoken。
- 配套墙板源码已按应用绑定语音续聊、详情刷新和确认目标；旧固件仍可能固定 dsh.*。三平台墙板交互尚需 ESP-IDF 编译及实机联调。
- 本机 HTTP 兼容路由要求管理令牌；旧版未经认证的 LAN 模式不直接兼容，推荐既有出站 WS 模式。

回退时退出 Go 桥、关闭它的登录启动，再重新运行旧桥。旧配置不变，新桥数据保留在独立目录。

## 验证

```sh
go test ./... -timeout 90s
go vet ./...
go test -race ./... -timeout 120s   # 需要支持 race 的本机工具链
go run ./cmd/package
```

测试使用本地模拟上游，不调用真实账号、不创建收费 API 请求、不注册启动项。覆盖凭证/CSRF/Host 检查、跨平台会话拒绝、OpenAI 异步续聊、WorkBuddy ACP、DSH 精确会话/陈旧审批、串接 zstd 日志、Host 原样包哈希和单实例锁。

GitHub Actions 配置三系统原生测试及六目标打包。配置工作流不代表已经执行 CI。真实账号、macOS/Linux 启动、ARM 实机和墙板操作仍需验证；使用新版墙板入口需要自行编译并更新固件。
