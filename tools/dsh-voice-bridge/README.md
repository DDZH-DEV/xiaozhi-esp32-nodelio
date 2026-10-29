# dsh-voice-bridge

让小智墙板把任务交给本机 DSH 工作台：派活、查进度、确认审批。

实现计划见 [PLAN.md](PLAN.md)。

## 角色

| 角色 | 干什么 |
|------|--------|
| 墙板 | 语音/触控 invoke；显示 list 卡；点允许/拒绝 |
| Host | 安装市场包、代 invoke、MQTT `kind=notice`（旧 `skill_hint` 兼容）；公网拓扑下收桥出站 WS |
| 本桥 | LAN HTTP Envelope 上游 + 可选出站 `host-ws`；调 DSH Desktop JSON-RPC |
| DSH Desktop | 真正执行 |

**禁止**把 DSH `authToken` 写入固件或市场包。

## 模式（`DSH_BRIDGE_MODE` / 配置页 `mode`）

| 值 | 行为 |
|----|------|
| `both`（默认） | LAN 听 `0.0.0.0:8790` + 有 token 时出站连 Host |
| `lan` | 仅 LAN HTTP（家庭 Host 同机/同网） |
| `host_ws` | 本机仅 `127.0.0.1:8790`（供 WS 回环）+ 出站连公网 Host |

## 配置页（推荐）

桥启动后浏览器打开：

```text
http://127.0.0.1:8790/config
```

可改 mode、Host WS / token、边沿监视、HTTP hint 回落、**新建会话默认模型/权限/Agent 模式**等。点「保存并应用」写入 `bridge-config.json` 并**热生效**（出站 WS 重连、监视开关）。  
**监听 host/port** 改完需重启进程。密钥类字段留空表示保留原值。样例见 `bridge-config.example.json`（真实 `bridge-config.json` 已 gitignore）。

新建会话：DSH 的 `session.createAndStart` 要求 AA 侧已有模型；桥会按配置注入 `selections` + `agentPreset`。未配 `default_model` 时用目录第一个可用模型。有空闲会话时仍优先 `startTurn` 复用。

API：`GET|PUT /nodelio/dsh/config`。

## 家庭部署（最短路径 · LAN）

### 1. 启动桥

```bat
cd tools\dsh-voice-bridge
start-bridge.bat
```

要求：DSH Desktop 已开且远程控制已登录（存在 `~/.agents-anywhere/dsh-bridge/endpoint.json`）。

默认 `mode=both`，监听 `0.0.0.0:8790`。也可先开配置页填好再保存。Host 与桥同机时，包内 `invoke.url` 用 `http://127.0.0.1:8790/...`。

**代码热更新**：默认开（Node `--watch`）。关闭：`set DSH_BRIDGE_HOT=0`。

可选边沿推送（LAN 可用 HTTP hint；公网优先 WS hint）：

```bat
set DSH_BRIDGE_WATCH=1
set NODELIO_HOST_URL=http://127.0.0.1:8080
set NODELIO_PANEL_DEVICE_ID=你的墙板ID
set NODELIO_PANEL_DEVICE_KEY=设备密钥
start-bridge.bat
```

### 2. Host（LAN）

家庭 `on_prem`（默认）已自动允许私网出站，一般**不用**再设环境变量。  
仅当 Host 是 `saas`、或被设了 `NODELIO_OUTBOUND_ALLOW_PRIVATE=0` 时，才需显式 `=1`，否则代理/安装拉 `192.168.*` 会报 `outbound blocked`。

## 公网 Host（出站 WebSocket · 免穿透）

Host 在云上、DSH 在家里时：**不要**给家里开入站端口。桥主动连 Host。

1. 手机 APP（已登录、已连上 Host）→ **设置 → 账号偏好 → 生成桥凭证** → 复制。
2. 电脑桥 `http://127.0.0.1:8790/config`：粘贴 `host_ws` + `bridge_token` → 保存并应用。
3. 角标「Host WS 已连接」后即可装 `dsh.*` / 派活。Host **不必**能访问家里 `192.168.*`。

（高级）桥配置页也可填 Owner 会话代签；或环境变量 `BRIDGE_TOKEN`。）

### 3. APP

1. 添加市场源（二选一）：
   - **公网 / 蜂窝推荐**：设置 → 市场源 →「家里 DSH」（需桥已在线；URL 为 `nodelio-dsh://marketplace.json`，Host 经 WS `fetch` 拉包）
   - **同网 LAN**：`http://<桥LAN>:8790/nodelio-dsh-market/marketplace.json`（桥日志 / `health.market_url`；H5 可直连）
2. 安装 `dsh.task` / `dsh.status` / `dsh.answer`（`dsh.answer` 仍须安装，但**不进墙板应用列表**，只给进度卡审批按钮用）
3. 在「DSH 派活」配置 **默认工作区**（如 `D:\xz25`）
4. 可选钉选到首页

### 4. 墙板：怎么派活 / 怎么看进度

进度卡一屏完成：顶栏 **横滑工作区 chip** + 下方 **当前区会话列表**（`display.filters` + list；非专用仪表盘页）。正文指令只靠语音。

| 你想做的 | 怎么说 / 怎么做 |
|----------|-----------------|
| 看进度 | 「查看任务」→ 顶栏工作区 + 会话列表（一屏约 4 条）；点 chip / 「第 N 个工作区」切换 |
| 在某工作区派活 | 点选工作区 chip 后说「让它…」 |
| 对某会话续聊 | 点「续聊」→ 开麦 → 说指令 → **屏上确认**（说「是」/点发送，或「不用」/点取消） |
| 直接派活 | 「让它…」（无武装则用 APP 默认工作区，复用该区第一个空闲会话） |
| 看末轮全文 | **点会话行** → 滚动纯文本（换行保留；图/表/代码收成文字）；顶栏刷新回列表 |
| 强制新建会话 | 「让它新建会话 + 要做的事」 |
| 裸说「派活」 | 只提示口令，不报缺参数 |
| 审批 | 二元：进度卡「允许 / 拒绝」；提问（每题都有选项，含多题）：点开任务 → 逐题「选择」，桥攒齐后一次提交；含填空题仍请在电脑答 |
| 刷新 | 点顶栏刷新 / 再说「查看任务」；开 `DSH_BRIDGE_WATCH=1` 可在完成/审批时 MQTT 推一次 |

**前提**：APP 给「DSH 派活」配好默认工作区（无武装时回落；打开进度时优先选中该区）；桥与 DSH Desktop 在跑。  
**非实时流**：派活 HTTP 立即返回列表，不等模型跑完；进度靠刷新或 watch 边沿 hint。

## CLI（调试）

```bash
node dsh-task.mjs list
node dsh-task.mjs show <id|标题片段> --speak
node dsh-task.mjs send "D:\xz25" "只回复一句话：测试通过。" --wait
```

## 端点

| 方法 | 路径 | 说明 |
|------|------|------|
| GET | `/nodelio/dsh/health` | 桥与 DSH 是否可达；含 `market_url` |
| GET | `/nodelio/dsh/workspaces` | 默认工作区候选（session cwd + 本机存在目录） |
| GET | `/nodelio/dsh/catalog` | 模型 / 权限 / Agent 模式（配置页下拉） |
| GET | `/nodelio-dsh-market/marketplace.json` | 市场索引（包同前缀） |
| POST | `/nodelio/dsh/task` | `{instruction, workspace?, session?}`；有 session 则对该会话 startTurn |
| GET/POST | `/nodelio/dsh/status` | 无 workspace=工作区列表；有=会话列表；再有 session=详情 |
| POST | `/nodelio/dsh/answer` | allow / deny / cancel（deny·cancel 会 `session.interrupt` 停本轮） |

## 已知限制

- 需要 DSH Desktop 在跑；退出即断
- token 仅本机；勿过公网
- 无任务卡原地刷新：靠再问进度、点刷新，或 Notice/`skill_hint` 单次 re-invoke（`needs_you`→action，`completed`→info）
- 列表 `density=comfortable`（约 4 行）；顶栏 `display.filters` 工作区 chip；行「续聊」/审批
- 点会话行 → `session` 详情 `body` 纯文本；刷新清 `detail_body` 重回列表
- `default_workspace`：派活无武装时回落；打开进度时优先选中该 chip
- 「续聊」本地 `arm` + 开麦；武装后下一句可裸说；无武装仍用「让它…」；`arm` 不打桥
- `spoken.audio`：进度列表**不下发**；详情仅当技能配置 `detail_tts=1` 且非静默轮询时按句读切段连续播（最多 6 段）；默认关闭以免详情 10s 刷新反复 TTS
- TTS：默认 `DSH_TTS_ENGINE=auto` → 优先微软 Edge 神经声（需 `pip install edge-tts`，音色 `DSH_TTS_VOICE` 默认 `zh-CN-XiaoxiaoNeural`）；否则 Windows SAPI 中文声。强制：`edge` / `sapi`
- `ttl_sec=0` 列表不自动关；`session.steer` 本机 unsupported
- Host 家庭须允许私网出站

## 校验

```bash
cd marketplace
npm install --no-save ajv@8 ajv-formats@3
node validate.mjs
node negative.mjs
```
