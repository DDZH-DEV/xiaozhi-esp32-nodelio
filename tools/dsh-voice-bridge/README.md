# dsh-voice-bridge

让小智（ESP32 墙板 / 语音助手）把任务交给 DSH 工作台执行，并能精确查询“那个任务现在到底在干什么”。

## 本目录是什么，不是什么

**是**：一个跑在 PC 上的命令行，证明 DSH 这一侧的接口是通的、任务状态是能精确读出来的。

**不是**：产品。它没有连小智、没有语音、没有屏幕、不常驻、关掉终端就没了。
`dsh-task.mjs` 将来是被那个常驻服务 `import` 的库，不是最终形态。

一句话：**插座装好了，插头还没接。**

## 目标形态（用户口述，待确认）

一条闭环，七个环节：

```
① 对着墙板说："让它查一下 xz25 那个构建为啥挂了"
        ↓ 小智 ASR + 意图判断
② 面板把这句话（不是音频）发给 PC 上一个常驻服务
        ↓
③ 服务把它变成 DSH 任务：工作区 D:\xz25，投进一个空闲会话
        ↓
④ DSH 开始干活——可能 30 秒，也可能 10 分钟、50 次工具调用
        ↓
⑤ 随时能问："它现在在干嘛？"
   → "第 3 步，正在跑 build.py，已经调了 12 次工具。"
        ↓
⑥ 中途 DSH 要审批 / 要追问
   → 面板弹一张卡，点一下，或者说"允许"
        ↓
⑦ 干完 → "构建挂了，原因是 flash 分区不够。"
```

三个角色的分工：

| 角色 | 是什么 | 干什么 |
|---|---|---|
| 小智 | 耳朵和嘴 | 收口语、播报结果 |
| 墙板 | 眼睛和手指 | 显示任务卡、点审批 |
| DSH | 手 | 真正干活 |

**核心命题不是“让 DSH 会说话”，而是“让一个 480×480 的墙板能看见并干预一个在 PC 上跑的长任务”。**
这是两件完全不同的事。

## “任务”在 DSH 里其实是三个东西

“工作区的任务”这个说法有歧义，可能指：

| 说法 | DSH 里的对象 | 典型时长 | 当前支持 |
|---|---|---|---|
| “帮我改个 bug” | **session turn** | 分钟级、几十次工具 | `show` 能查 |
| “跑一下构建” | **background job** | 秒到分钟、有 exit code | 只在日志里，没单独暴露 |
| “九点提醒我” | **scheduled reminder** | 定时触发 | 完全没做 |

如果指第一种，`show` 基本对；指第二种，还得答“那个 shell 还在跑吗、退出码多少”；指第三种，得另起一套。

## 可能理解偏的三处

1. **“精确知道”给成了 CLI，但可能要的是面板上的一屏。** CLI 再精确，墙板上也看不见。
2. **假设任务是一问一答。** 但 DSH 的任务会中途要审批、要追问、会跑很久。语音是一次性、同步的，这个错配没解决——而这恰恰是最难的部分。
3. **假设人就在 PC 前。** 如果要的是“在客厅说一句，出门后还能问进度”，那必须有常驻服务，而现在是前台脚本。

## 硬约束：桥只听 127.0.0.1

DSH 的桥只监听回环地址，墙板在 WiFi 上够不着。所以中间**必须有**一个跑在 PC 上的常驻服务：
对墙板监听 LAN，对 DSH 调本机 RPC。

## 为什么不用 MCP

DSH 的 MCP 客户端（`@deepseek-ai/dsh-mcp-client`）只接受 **stdio** 和 **streamable-http**。
小智固件这边 MCP 是**设备当 server**，而且 JSON-RPC 塞在 WebSocket/MQTT 信封里
（见 `docs/mcp-protocol.md`）。两者方向相反，中间必须有一层桥。

但 DSH Desktop **已经自带**一个本机 JSON-RPC 桥，就是「远程控制 / Agents Anywhere」用的那个：

```
~/.agents-anywhere/dsh-bridge/endpoint.json   { host, port, token }
```

它是换行分隔的 JSON-RPC 1.x，`initialize` 用 `authToken` 认证。本目录直接用它，
**不需要额外起服务、不需要改固件、不需要 MCP**。

## 已验证的能力

| 方法 | 作用 |
|---|---|
| `session.list` | 列出所有会话：`cwd`、`title`、`metadata.live` |
| `session.getState` | 实时状态：`status`、模型、权限、`cwd` |
| `session.getSnapshot` | 时间线条目（turn/message/tool，已归一化） |
| `session.getNotices` | 待处理的通知 / 审批 |
| `session.startTurn` | 往已有会话追加一轮 |
| `session.createAndStart` | 建新会话并立刻开始 |
| `session.interrupt` | 中断当前轮 |
| `session.respondInteraction` | 回答审批 / 提问 |
| `session.listCommands` | 可用的斜杠命令（本机只开放 `/compact`） |

## 一个“任务”的精确结构

DSH 的落盘日志（`~/.dsh/sessions/--<slug>--/<session-id>/session.v4.jsonl.zstd`）
是**多个 zstd 帧拼接**的（每次追加一帧），所以必须按魔数 `28 b5 2f fd` 切帧再解。
单帧 `zstdDecompressSync` 只能读到第一帧——这是第一个坑。

解出来是一行一个事件：

```
seq 10  turn/start       {turn:1}
seq 13  step/start       {turn:1, step:1}
seq 14  system/message
seq 26  assistant/message {reasoning, tool-call:read, tool-call:pwsh, ...}
seq 27  tool/call        {callId, name, arguments}
seq 28  tool/result      {callId, message.content[].text}
seq 35  step/end
seq 36  step/start       {turn:1, step:2}
...
seq 208 turn/end         {reason:{kind:"completed"}}
```

**层级：session → turn → step → tool/call。**
一个 step = 一次 LLM 调用 + 它发出的那批工具。`tool/result` 里没有 `status`，
成功与否要么看 `job/settled.status`，要么看结果文本里的 `[exit code: N]`——第二个坑。

## 用法

```bash
node dsh-task.mjs list                      # 按工作区分组列出所有任务
node dsh-task.mjs show <id|标题片段>          # 折叠成 8 个字段
node dsh-task.mjs show <id> --speak         # 只输出一句可直接 TTS 的话
node dsh-task.mjs show <id> --json          # 给程序消费
node dsh-task.mjs send "D:\xz25" "把 mcp_server 里的工具列出来" --wait
node dsh-task.mjs interrupt <id>
```

`send --wait` 是语音场景的主入口：投递 → 阻塞 → 返回那一句答复。已实测：

```
$ node dsh-task.mjs send "D:\SITES\XTOOLS" "只回复一句话：端到端等待测试通过。" --wait
queued into session-ef5745e3-... (查找未更新指标清空点)
端到端等待测试通过。
```

`show` 的实际输出：

```
workspace   D:\xz25
session     session-b2000c6b-490c-4996-bea1-c51a8047aab7
title       DeepSeek dsh工作台硬件对接方案
status      running
progress    turn 2 / step 52
tools       95 calls, last=pwsh (running)
model       deepseek-v4.1-flash / high
permission  danger-full-access
speak       第 52 步，正在用 pwsh 干活。
```

## 三个实现上的坑（都踩过了）

1. **zstd 是多帧拼接**。DSH 每次追加写一帧，`zstdDecompressSync` 只解第一帧。
   必须按魔数 `28 b5 2f fd` 切帧，再逐帧解。否则你只能读到 `session` 头一行。
2. **`tool/result` 没有 status**。成功与否只能看结果文本里的 `[exit code: N]`
   或 `Error:` 前缀，或者 `job/settled.status`。
3. **等待必须“先记基线再发”**。如果先 `send` 再 `wait`，而这一轮很快结束
   （比如模型只回一句话），`wait` 会从新的 turn 数开始数，永远等不到下一轮。
   所以 `send --wait` 在投递**之前**读一次 turn 计数。

## 语音侧的任务模型

只有 8 个字段，全部有真实来源，没有一个靠猜：

| 字段 | 来源 |
|---|---|
| `workspace` | `session.cwd` |
| `sessionId` | `session.externalSessionId` |
| `title` | `session/title` 事件 |
| `status` | `session.getState.status` |
| `turn` / `step` | `turn/start` / `step/start` |
| `lastTool` / `toolCalls` | `tool/call` 累计 |
| `needsYou` | `status ∈ {waiting_approval, waiting, pending}` |

## 已知限制

- **需要 DSH Desktop 在跑**。桥是 Desktop 进程的一部分，Desktop 退出就断。
- **需要「远程控制」已登录**。没登录时 `endpoint.json` 不存在或握手被拒。
- **token 是本机长期凭据**。它等于对本机 DSH 的完全控制权，别写进固件、别过公网。
- `session.getState` 只给状态，不给 turn/step。turn/step 必须读落盘日志，所以有几百毫秒延迟。
- `session.steer` 在本机 DSH 上返回 `supported:false`，只能 `startTurn` 追加或 `interrupt`。
- 本机 `/compact` 是唯一可用斜杠命令，别指望从语音触发别的命令。

## 待确认

**目标是否就是上面那条七环节闭环？**

- 若是 → 下一步是 LAN 常驻服务 + 墙板任务卡，不是继续加 CLI 功能。
- 若“精确知道”其实指别的东西（任务清单界面、历史任务复盘、或根本不想接语音只想接硬件）
  → 先停下来说清楚，再动手。

## 下一步（未实现）

1. **LAN 常驻服务**：对墙板开一个 LAN 端口，对 DSH 调本机 RPC。`dsh-task.mjs` 降级为它的库。
   → **`marketplace/server.mjs` 已经是这个服务的雏形**：三个端点、返回合法 Envelope、已跑通。
2. **墙板工具组**：`self.dsh.task` / `self.dsh.send` / `self.dsh.status`，
   走现有 `panel/skills/invoke` 转发到常驻服务。
3. **任务卡 UI**：480×480 上一屏显示 `title / turn / step / lastTool / needsYou`，
   审批时变成可点按钮。

备选（若要走 MCP 而非自建协议）：把三个动作包成 MCP 工具，用 `streamable-http`
暴露给 DSH 自己或其他 agent。注意 `@deepseek-ai/dsh-mcp-client` 只认 stdio 和
streamable-http，且 stdio 会起两次探测进程，串口类设备不能走 stdio。

## marketplace/ —— 作为应用商店插件上架

`marketplace/` 按 NodeLio Protocol v1 把上面这套包成三个应用（`dsh.task` / `dsh.status` /
`dsh.answer`），全部 JSON 用协议仓官方 schema 实跑校验通过，并已端到端投递成功。
详见 [marketplace/README.md](marketplace/README.md)。
