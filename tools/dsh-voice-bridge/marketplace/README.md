# marketplace/ — DSH 作为 NodeLio 应用商店插件

按 NodeLio Protocol v1 把语音桥上架成**三个应用**。全部 JSON 已用协议仓的官方 schema 实跑校验通过。

## 为什么是三个应用，不是一个

协议 v1 里一个应用 = 一次 `invoke` = 一个 Envelope。没有「任务」对象，没有进度字段，`invoke.timeout_ms` 上限 **30000**（`02 §2`）。DSH 任务常常跑几分钟，所以**不能同步等**。

于是拆成三个同步动作，每个都远低于 30 秒：

| app_id | 方法 | 超时 | 干什么 |
|---|---|---|---|
| `dsh.task` | POST | 15000 | 投递指令 + 返回任务清单（立即返回，不等执行完） |
| `dsh.status` | GET | 8000 | 查某工作区/某任务跑到第几步 |
| `dsh.answer` | POST | 8000 | 允许/拒绝挂起的审批 |

`dsh.status` 用 `GET` 是刻意的：协议 `04 §4` 规定 `GET` 只能用于**无副作用查询**，状态查询正好符合。

## 文件

```
marketplace.json                          市场索引（market_id: com.nodelio.dsh）
packages/dsh.task/1.0.0.json              应用包
packages/dsh.status/1.0.0.json
packages/dsh.answer/1.0.0.json
examples/envelope-dsh-task-list.json      list 卡 + spoken
examples/envelope-dsh-status.json
examples/envelope-dsh-answer.json
examples/envelope-dsh-error.json          status:error + error.code
examples/installed-app-list-panel.json    设备侧已装清单（含 fallback_card_type）
server.mjs                                三个端点的真实实现（返回真 Envelope）
validate.mjs                              正例校验（手写样例）
negative.mjs                              反证（确认校验器不是全通过）
verify-live.mjs                           校验**运行时真实产出**的 Envelope
```

## 跑校验

```bash
cd tools/dsh-voice-bridge/marketplace
npm install --no-save ajv@8 ajv-formats@3

node validate.mjs                                   # 9 个手写样例
node negative.mjs                                   # 6 个反例必须被拒

# 真实跑一遍
node server.mjs --port 8791 &
node verify-live.mjs --port 8791 --workspace "D:\xz25"
```

协议仓不在本仓库，脚本按以下顺序找：

1. 命令行参数 `node validate.mjs D:\SITES\NodeLio\Protocol`
2. `NODELIO_PROTOCOL_ROOT`
3. `../../../../SITES/NodeLio/Protocol`（本机布局）

实测输出：

```
ok    marketplace.json  [marketplace.schema.json]
ok    packages/dsh.task/1.0.0.json  [app-package.schema.json]
ok    packages/dsh.status/1.0.0.json  [app-package.schema.json]
ok    packages/dsh.answer/1.0.0.json  [app-package.schema.json]
ok    examples/envelope-dsh-task-list.json  [content-envelope.schema.json]
ok    examples/envelope-dsh-status.json  [content-envelope.schema.json]
ok    examples/envelope-dsh-answer.json  [content-envelope.schema.json]
ok    examples/envelope-dsh-error.json  [content-envelope.schema.json]
ok    examples/installed-app-list-panel.json  [installed-app-list.schema.json]

all schemas passed
```

反证也过了——大写能力名、`1.0` 版本号、缺 `invoke`、`timeout_ms: 120000`、`status:error` 缺 `error`、两段 `market_id`，六类全被拒。

`verify-live.mjs` 把**运行时真实返回**的 5 种 Envelope 也送进了 schema：

```
ok    GET /nodelio/dsh/status  (status=ok, content=2)
      spoken: 第 37 步，正在用 pwsh 干活。
ok    GET /nodelio/dsh/status (no match)  (status=ok, content=0)
      spoken: 没找到这个任务。
ok    POST /nodelio/dsh/task (missing instruction)  (status=error, content=0)
ok    POST /nodelio/dsh/answer (missing notice)  (status=error, content=0)
ok    GET /nope (unknown route)  (status=error, content=0)

all live envelopes valid
```

端到端也验过一次真的投递：往 `D:\SITES\XTOOLS` 发 `"只回复一句话：商店插件端到端测试通过。"`，
8 秒后 `dsh.status` 回报 `idle · turn 6`，回读落盘日志拿到答复 `商店插件端到端测试通过。`

## 三个设计取舍

### 1. `dsh.task` 的 `requires` 只要 `content.list`

没要 `content.music_list`、没要 `media.*`。`00 §3.5` 明确说 `media.*` 不参与安装判定，而且“能装”和“能播”是两回事。这里只需要画列表 + 说一句话。

### 2. 审批用独立 app，不用 `actions[].local`

`03 §4.1` 的 `local` 白名单只有 `media.*` 和 `web.open_url`，全是媒体动作。硬塞一个 `dsh.approve` 进去会违反白名单（`08 §8` 明令禁止“自创字符串当脚本”）。所以审批走 `run: "invoke"` 指向 `dsh.answer` —— 这是协议里唯一合规的“按钮调另一个应用”表达。

### 3. `fallback` 一律 `plain_result`

`02 §2` 说 `fallback.card_type` 是“端不支持默认卡时的首选降级”，且**必须**写进设备侧清单（`04 §3.2`）。`plain_result` 是所有端（含无屏音箱）都能落地的最后一档，最安全。

## 一个必须知道的限制

**这套东西做不到“任务卡原地刷新”。**

协议没有“按卡 id 更新”的概念，`ttl_sec` 只是缓存建议（`05 §3`）。每次问进度都是一次新 invoke、出一张新卡。

想做到“卡自己变”，只有两条路：

- 改固件认 `x_dsh_task` 扩展（`07 §6` 允许，但端**必须**忽略未知扩展 → 等于没写，除非改固件）
- 走 `panel/notify` 平行通道（控家通道，`04 §9` 明确说它和技能通道隔离）

我按你说的先做合规路线，这两条没动。

## 上架前要改的

`REPLACE-ME.example.com` 出现 5 次（3 个包的 `invoke.url` + 2 个 `package_url`）。真实部署时替换成 HTTPS 地址，并给每个包补 `package_sha256`（`01 §1.2`：不带则 Host 允许安装但记 warning，**不得**当作可信来源）。
