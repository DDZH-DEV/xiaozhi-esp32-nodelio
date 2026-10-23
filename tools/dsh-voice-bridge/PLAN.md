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
| 10 | 公网 Host 经桥 WS 拉市场/装包（见下「阶段 10」） | **10a/10b 已落地**（10c 缓存可选未做） |

## 已落地路径

| 项 | 路径 |
|----|------|
| 桥 LAN + WS | `marketplace/server.mjs`、`lib/host-ws.mjs`、`start-bridge.bat` |
| Host WS | `SERVER/websocket/dsh_bridge.go`；`GET /ws/dsh-bridge` |
| Token | `POST/GET/DELETE /api/owner/bridge-token(s)`；migration `add_bridge_tokens` |
| Invoke | `device_handler_skill.go`：`dsh.*` 且桥在线 → WS，否则 LAN URL |
| 装包 fetch | Host `DshBridge.Fetch` + 桥 `host-ws` `type=fetch`；虚拟源 `nodelio-dsh://` |
| 固件 | list `run:invoke`；`arm` 本地武装；「让它」 |

## 仓库边界

| 仓库 | 改动 |
|------|------|
| xz25 | bridge、marketplace、本 PLAN |
| NodeLio | SERVER WS/token/invoke、home-deploy |
| Protocol | 不改 v1 |

---

## 阶段 10：公网装 DSH 插件（Host 够不到桥 LAN）

### 问题

- 官方 `nodelio-market` **不再**带 `dsh.*`；包只在桥 `GET /nodelio-dsh-market/**`。
- **Invoke** 已走桥出站 WS，云 Host 不必进私网。
- **安装**仍要 Host（或代理）`GET package_url`；云 Host 拉不到 `http://192.168.*:8790/...` → 装不上、面板无入口。
- 手机蜂窝网时 APP 也直连不了家里桥；回落 `/marketplace/proxy` 同样失败。

### 原则

| 要 | 不要 |
|----|------|
| 包**唯一源**仍在桥（与 LAN 市场同源） | 再塞回 Host `static/nodelio-market`（版本双份） |
| 复用现有出站 WS（与 invoke 同通道） | 给家里开入站 / 穿透 |
| 桥离线时安装明确失败（`bridge_offline`） | 静默装残缺包 |
| `invoke.url` 可继续写 `127.0.0.1:8790`（运行时优先 WS） | 为安装改协议 Envelope |

### 选定方案：虚拟市场源 + WS `fetch`

```text
APP 市场源：nodelio-dsh://marketplace.json   （或 Host 合成 URL）
        │
        ▼
Host 发现「桥市场」且该 owner 桥在线
        │  type=fetch  path=/nodelio-dsh-market/...
        ▼
桥读本地 marketplace 文件 → result body（JSON 原文）
        │
        ▼
Host 代理给 APP / 写入 panel_skills（安装同今日逻辑）
```

**不选**：Host 内嵌整包镜像（易漂移）；仅文档「先 LAN 再换云」（体验差）。

### 协议增补（WS 帧，与 invoke 并列）

| 方向 | `type` | 字段 | 说明 |
|------|--------|------|------|
| Host→桥 | `fetch` | `request_id`, `path`（仅允许 `/nodelio-dsh-market/` 前缀） | 读桥磁盘/内存市场文件 |
| 桥→Host | `result` | `request_id`, `status_code`, `body`（JSON 或 `{raw, content_type}`） | 与 invoke result 同形即可 |
| 超时 | — | 默认 8s | 小于 invoke |

路径白名单：**禁止** `fetch` 任意本地路径（防 token 被滥用读盘）。

### Host 行为

1. **虚拟源**：`GET /api/market-sources` 在 `bridge_online` 时自动露出一条（或 APP 一键添加）  
   - `market_url` = `nodelio-dsh://marketplace.json`（或 `https://<host>/api/dsh-market/marketplace.json` 薄封装）  
   - `market_id` = `com.nodelio.dsh`（与桥包一致）  
   - `market_url_origin` 稳定键：`nodelio-dsh`（勿用 LAN IP，避免换 IP 冲突）
2. **`MarketplaceProxy` / `fetchAppPackage`**：若 URL 为虚拟源或 `origin=nodelio-dsh`，且桥在线 → `DshBridge.Fetch(owner, path)`；否则保持现 HTTP 出站。
3. **安装**：`OwnerInstallSkill` 同上；校验 sha256 / schema 不变。装完 `invoke_url` 仍为包内 `http://127.0.0.1:8790/...`（运行已优先 WS）。
4. **桥离线**：代理/安装返回 `bridge_offline` + 中文「请先在电脑打开 DSH 桥并连上 Host」。

### APP 行为

1. 技能页 / 市场源：桥在线时显示「添加家里 DSH 市场」→ 写入虚拟源（不必手填 LAN）。
2. 浏览/安装：仍走现有 `fetchMarketplace` → 失败或虚拟 URL 走 Host 代理（Host 内部转 WS）。
3. 可选：已装且桥离线时列表灰显 + toast，与 invoke 失败文案一致。

### 桥行为

1. `host-ws.mjs` 处理 `fetch`：映射 `path` → `marketplace/` 下文件（与 HTTP `/nodelio-dsh-market` 同源）。
2. `package_url` 在索引里可继续是相对路径或本桥绝对 URL；Host 安装时把包 URL **归一成** `nodelio-dsh://packages/dsh.status/1.0.0.json` 再 Fetch，避免云去打 LAN。

### 分期落地

| 子阶段 | 内容 | 验收 |
|--------|------|------|
| **10a** | WS `fetch` + Host `Fetch` + 安装/proxy 走桥 | **已做** |
| **10b** | 虚拟市场源 / APP「家里 DSH」+ `bridge_online` | **已做** |
| **10c**（可选） | Host 装后缓存包 JSON（按 sha），桥短离线仍能「重装同版本」 | 未做 |

### 明确不做（本阶段）

- 扫码配对 / 一键装桥 exe（仍阶段 9 延后）
- 把 `dsh.*` 拷回 `NodeLioAPP/static/nodelio-market`
- 固件侧装市场

### 文档同步（实现时）

- `home-deploy.md` 公网节：删「只能先 LAN 装」；改为「桥在线 → APP 添加 DSH 市场 → 安装」
- 本 README 公网节同改
