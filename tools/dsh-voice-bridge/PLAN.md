# DSH × 墙板交互开发计划

> 实现跟进文档。权威进度以本文件 + 仓库代码为准。

## 目标与边界

**目标**：客厅墙板（xz25）语音/触控把自然语言任务交给本机 DSH，能问进度、能审批，结果用 Content Envelope 上屏 + TTS。

**不做**：固件内嵌 DSH RPC / 解 session 日志；专用「DSH 仪表盘」页（LVGL 新页）；短周期轮询进度；本阶段不做 APP 一键装桥 / 扫码配对。

**部署**：

| 拓扑 | 路径 |
|------|------|
| 家庭 on_prem（Host 与桥同机/同 LAN） | Host HTTP → `invoke.url`（LAN `:8790`） |
| 公网 Host + 家用 PC | 桥**出站** `WSS /ws/dsh-bridge` → Host 代 invoke（**无需穿透**） |

```text
# LAN
墙板 → Host invoke → LAN bridge → DSH Desktop

# 公网
墙板 → Host invoke →（桥已在线）WS invoke → 家桥回环本机 HTTP → DSH
桥 ──WSS──► Host（register / result / hint）
```

## 验收场景

1. 说「让它查一下 xz25 构建为啥挂了」→ 投递成功 spoken + list 卡
2. 说「查看任务」→ 顶栏横滑工作区 chip + 当前区会话；点 chip / 「第 N 个工作区」切换
3. 点「续聊」→ 开麦；直接说要做的事（武装 bare）
4. 选好工作区 chip 后「让它…」→ 落在该 cwd
5. DSH 挂起审批 → 屏上可点允许/拒绝
6. 裸说「派活」→ 只提示口令
7. 公网：桥带 `BRIDGE_TOKEN` 连上后，墙板派活/进度不依赖 Host 打家里 IP
8. 断桥 → Envelope `bridge_offline` spoken 明确

## 阶段状态

| 阶段 | 内容 | 状态 |
|------|------|------|
| 0–5 | LAN 桥、市场包、filters、续聊、hint HTTP | 完成 |
| 6 | Host `/ws/dsh-bridge` + `bridge_tokens` + invoke WS 转发 | 完成 |
| 7 | 桥 `host-ws.mjs` + `DSH_BRIDGE_MODE` + hint 经 WS | 完成 |
| 8 | README / home-deploy 公网拓扑 | 完成 |
| 9 | APP 装桥 / 扫码配对 UX | **延后** |

## 已落地路径

| 项 | 路径 |
|----|------|
| 桥 LAN + WS | `marketplace/server.mjs`、`lib/host-ws.mjs`、`start-bridge.bat` |
| Host WS | `SERVER/websocket/dsh_bridge.go`；`GET /ws/dsh-bridge` |
| Token | `POST/GET/DELETE /api/owner/bridge-token(s)`；migration `add_bridge_tokens` |
| Invoke | `device_handler_skill.go`：`dsh.*` 且桥在线 → WS，否则 LAN URL |
| 固件 | list `run:invoke`；`arm` 本地武装；「让它」 |

## 仓库边界

| 仓库 | 改动 |
|------|------|
| xz25 | bridge、marketplace、本 PLAN |
| NodeLio | SERVER WS/token/invoke、home-deploy |
| Protocol | 不改 v1 |
