# DSH × 墙板交互开发计划

> 实现跟进文档。权威进度以本文件 + 仓库代码为准。

## 目标与边界

**目标**：客厅墙板（xz25）语音/触控把自然语言任务交给本机 DSH，能问进度、能审批，结果用 Content Envelope 上屏 + TTS。

**不做**：固件内嵌 DSH RPC / 解 session 日志；专用「DSH 仪表盘」页（LVGL 新页）；短周期轮询进度；公网官方云直连家里 `127.0.0.1`（固定为**家庭自建 Host**）。

**部署默认**：Host 与 DSH 同局域网；`NODELIO_OUTBOUND_ALLOW_PRIVATE=1`；桥监听 LAN（非仅 loopback）；token 只留在 PC。

```text
墙板 → Host panel/skills/invoke → LAN bridge (server.mjs) → DSH Desktop JSON-RPC
     ← Content Envelope ← 校验裁剪 ←
```

## 验收场景

1. 说「让它查一下 xz25 构建为啥挂了」→ 投递成功 spoken + list 卡
2. 说「查看任务」→ 顶栏横滑工作区 chip + 当前区会话；点 chip / 「第 N 个工作区」切换；TTS 请说第几个任务
3. 会话说「第 N 个」或点「续聊」→ 提示后说「让它…」→ 对该 session `startTurn`
4. 选好工作区 chip 后「让它…」→ 落在该 cwd
5. DSH 挂起审批 → 屏上可点允许/拒绝（优先占满 2 槽）
6. 裸说「派活」→ 只提示口令，不出「缺少 instruction」红卡
7. 任务结束再问 → spoken + list subtitle 带末条助手摘要或失败句

## 阶段状态

| 阶段 | 内容 | 状态 |
|------|------|------|
| 0 | 本计划落盘 + panel-skills 交叉引用 | 完成 |
| 1 | LAN 桥 + 市场包 + 派活/查进度 | 完成 |
| 2 | status actions + 固件 invoke 按钮 | 完成 |
| 3 | skill_hint notify 推送 | 完成 |
| 4 | README / home-deploy 文档 | 完成 |
| 5 | 工作区横滑 filters + 会话列表 + 续聊 arm | 完成 |

## 已落地路径

| 项 | 路径 |
|----|------|
| 桥 | `marketplace/server.mjs`、`start-bridge.bat` |
| 市场（工具仓） | `marketplace/marketplace.json` + `packages/*` |
| 市场（桥吐源） | `GET :8790/nodelio-dsh-market/**` |
| Host | skill invoke 透传 workspace/session；`VersionNum` 见 product.go |
| 固件 | list `run:invoke`；`arm` 本地武装；「让它」带 workspace/session |

## 仓库边界

| 仓库 | 改动 |
|------|------|
| xz25 | bridge、marketplace、本 PLAN；阶段 2/3/5 固件 |
| NodeLio | SERVER skill invoke、home-deploy / panel-skills 文档 |
| Protocol | 不改 v1 |
