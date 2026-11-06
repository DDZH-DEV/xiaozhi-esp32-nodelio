package bridge

import (
	"context"
	"errors"
	"fmt"
	"io"
	"path/filepath"
	"sort"
	"strings"
	"sync"
	"time"
)

type codexPage struct {
	Workspace, Cursor, Previous string
	Number                      int
}
type Codex struct {
	e          *Engine
	slots      chan struct{}
	sendMu     sync.Mutex
	mu         sync.Mutex
	identity   string
	workspaces map[string]string
	pages      map[string]codexPage
}

func NewCodex(e *Engine) *Codex {
	return &Codex{e: e, slots: make(chan struct{}, 2), workspaces: map[string]string{}, pages: map[string]codexPage{}}
}

func (c *Codex) connect(ctx context.Context) (*codexReader, ProviderConfig, string, func(), error) {
	p := c.e.Config.Snapshot().Config.Providers["codex"]
	p.CodexHome = codexHome(p)
	if !p.Enabled {
		return nil, p, "", nil, errors.New("请先在电脑桥启用 Codex 桌面")
	}
	select {
	case c.slots <- struct{}{}:
	default:
		return nil, p, "", nil, errors.New("桌面连接繁忙，请稍后刷新")
	}
	release := func() { <-c.slots }
	path, err := codexExecutable(p)
	if err != nil {
		release()
		return nil, p, "", nil, err
	}
	identity := fingerprint(path, p.CodexHome)
	c.mu.Lock()
	if c.identity != identity {
		c.identity = identity
		c.workspaces = map[string]string{}
		c.pages = map[string]codexPage{}
	}
	c.mu.Unlock()
	readCtx, cancel := context.WithTimeout(ctx, 20*time.Second)
	r, err := openCodexReader(readCtx, path, p)
	if err != nil {
		cancel()
		release()
		return nil, p, "", nil, err
	}
	return r, p, path, func() { r.close(); cancel(); release() }, nil
}

func (c *Codex) Health(ctx context.Context) Health {
	r, p, path, close, err := c.connect(ctx)
	if err != nil {
		return Health{State: "error", Message: err.Error(), Capabilities: []string{}}
	}
	defer close()
	_, err = r.call("thread/list", codexListParams("", "", 1))
	if err != nil {
		return Health{State: "error", Message: err.Error(), Capabilities: []string{}}
	}
	message := "本地历史可用 · 续聊排队交给桌面"
	if r.shared {
		message = "已连接桌面服务 · 续聊排队交给桌面"
	}
	if err := codexQueueAvailable(ctx, path, p); err != nil {
		return Health{State: "ready", Message: "会话可读 · CLI 不支持续聊队列，请更新 CLI", Capabilities: []string{"workspaces", "sessions", "history"}}
	}
	return Health{State: "ready", Message: message, Capabilities: []string{"workspaces", "sessions", "history", "queue"}}
}

func codexListParams(workspace, cursor string, limit int) map[string]any {
	p := map[string]any{"limit": limit, "modelProviders": []string{}, "sourceKinds": []string{"cli", "vscode", "appServer"}, "sortKey": "updated_at", "useStateDbOnly": true}
	if workspace != "" {
		p["cwd"] = workspace
	}
	if cursor != "" {
		p["cursor"] = cursor
	}
	return p
}

func (c *Codex) workspace(path string) string {
	if path == "" {
		return ""
	}
	ref := "ws-" + fingerprint(path)
	c.mu.Lock()
	defer c.mu.Unlock()
	if len(c.workspaces) >= 512 {
		for key := range c.workspaces {
			delete(c.workspaces, key)
			break
		}
	}
	c.workspaces[ref] = path
	return ref
}

func (c *Codex) task(thread map[string]any) Task {
	title := str(thread["name"])
	if title == "" {
		title = clip(str(thread["preview"]), 80)
	}
	if title == "" {
		title = "未命名会话"
	}
	state := "history"
	switch str(obj(thread["status"])["type"]) {
	case "active":
		state = "running"
	case "systemError":
		state = "failed"
	}
	return Task{ID: str(thread["id"]), Provider: "codex", Title: title, Body: clip(str(thread["preview"]), 600), Status: state, Workspace: c.workspace(str(thread["cwd"]))}
}

func validCodexThread(id string) bool {
	if len(id) != 36 {
		return false
	}
	for i, ch := range id {
		if i == 8 || i == 13 || i == 18 || i == 23 {
			if ch != '-' {
				return false
			}
		} else if !strings.ContainsRune("0123456789abcdefABCDEF", ch) {
			return false
		}
	}
	return true
}

func (c *Codex) read(r *codexReader, id, account string) (Task, error) {
	if !validCodexThread(id) {
		return Task{}, errors.New("无效的桌面会话，请从列表重新选择")
	}
	doc, err := r.call("thread/read", map[string]any{"threadId": id, "includeTurns": false})
	if err != nil {
		return Task{}, err
	}
	thread := obj(doc["thread"])
	if str(thread["id"]) != id {
		return Task{}, errors.New("桌面返回了不匹配的会话")
	}
	t := c.task(thread)
	turns, err := r.call("thread/turns/list", map[string]any{"threadId": id, "limit": 3, "itemsView": "full", "sortDirection": "desc"})
	if err != nil {
		return Task{}, err
	}
	marker, hasMarker := c.e.State.Get("codex-queue-" + account + "-" + id)
	var body []string
	var userIDs []string
	canTrackIDs := true
	observed := false
	rows := arr(turns["data"])
	for i := len(rows) - 1; i >= 0; i-- {
		for _, value := range arr(obj(rows[i])["items"]) {
			item := obj(value)
			switch str(item["type"]) {
			case "userMessage":
				itemID := str(item["id"])
				if itemID == "" || len(userIDs) >= 128 {
					canTrackIDs = false
				} else {
					userIDs = append(userIDs, fingerprint(itemID))
				}
				var parts []string
				for _, entry := range arr(item["content"]) {
					if str(obj(entry)["type"]) == "text" {
						parts = append(parts, str(obj(entry)["text"]))
					}
				}
				text := strings.Join(parts, "\n")
				body = append(body, "你："+text)
				// Only a new message ID acknowledges delivery. Repeating an old
				// instruction, or steering a turn that started earlier, is safe.
				if hasMarker && marker.RemoteID != "" && itemID != "" && fingerprint(text) == marker.PreviousResponse && !strings.Contains(marker.RemoteID, ","+fingerprint(itemID)+",") {
					observed = true
				}
			case "agentMessage":
				body = append(body, "助手："+str(item["text"]))
			}
		}
	}
	if canTrackIDs {
		t.RemoteID = "," + strings.Join(userIDs, ",") + ","
	}
	text := []rune(strings.Join(body, "\n\n"))
	if len(text) > 5500 {
		text = append([]rune("…\n"), text[len(text)-5500:]...)
	}
	t.Body = string(text)
	if !r.shared {
		t.Body = "本地会话记录 · 实时状态与审批请在桌面确认\n\n" + t.Body
		t.Status = "history"
	}
	if input, ok := thread["canAcceptDirectInput"].(bool); ok && !input {
		t.Status = "readonly"
		t.Body = "此会话不接受直接输入，请在桌面处理。\n\n" + t.Body
	}
	if hasMarker && (marker.Status == "queued" || marker.Status == "unknown") && !observed {
		t.Status = marker.Status
		t.Body = "续聊" + statusLabel(marker.Status) + "；请在电脑打开该会话。尚未在最近记录中看到本次消息，请勿重复发送。\n\n" + t.Body
		t.Notices = []Notice{{ID: fingerprint(marker.ID, marker.RequestID, marker.PreviousResponse), Title: "请先在桌面确认消息已处理或移除。下方按钮仅解除桥的重复发送保护，不会撤回队列消息。", Options: []Option{{ID: "queue-checked", Title: "已在桌面核实"}}}}
	}
	if observed && marker.PreviousResponse != "" {
		marker.Status = "history"
		marker.PreviousResponse = ""
		if err = c.e.State.PutIfRequest(marker, marker.RequestID); err != nil {
			return Task{}, err
		}
	}
	return t, nil
}

func (c *Codex) List(ctx context.Context, q Request) ([]Task, error) {
	r, p, _, close, err := c.connect(ctx)
	if err != nil {
		return nil, err
	}
	defer close()
	if q.Session == "" {
		return nil, errors.New("请先选择桌面工作区和会话")
	}
	t, err := c.read(r, q.Session, fingerprint(p.CodexHome))
	if err != nil {
		return nil, err
	}
	return []Task{t}, nil
}

func (c *Codex) Send(ctx context.Context, q Request) (Task, error) {
	if q.NewSession || q.Session == "" {
		return Task{}, errors.New("请先在桌面创建会话，再从面板选择该会话续聊")
	}
	if !c.sendMu.TryLock() {
		return Task{}, errors.New("正在提交桌面消息，请稍后刷新")
	}
	defer c.sendMu.Unlock()
	r, p, path, close, err := c.connect(ctx)
	if err != nil {
		return Task{}, err
	}
	defer close()
	account := fingerprint(p.CodexHome)
	t, err := c.read(r, q.Session, account)
	if err != nil {
		return Task{}, err
	}
	if t.Status == "readonly" {
		return Task{}, errors.New("此会话不接受直接输入，请在桌面处理")
	}
	if t.Status == "queued" || t.Status == "unknown" {
		return Task{}, errors.New("上次续聊仍待桌面处理或结果待核实，请在电脑确认后刷新")
	}
	if err = codexQueueAvailable(ctx, path, p); err != nil {
		return Task{}, err
	}
	// Persist before running the queue command. Crashes/timeouts never auto-resend.
	marker := Task{ID: "codex-queue-" + account + "-" + q.Session, Provider: "codex", Title: t.Title, Status: "unknown", Account: account, RemoteID: t.RemoteID, PreviousResponse: fingerprint(q.Instruction), RequestID: NewID()}
	if err = c.e.State.Put(marker); err != nil {
		return Task{}, err
	}
	cmd := codexCommand(ctx, path, p, "queue", "--thread", q.Session, "--message", q.Instruction)
	cmd.Stdout = io.Discard
	cmd.Stderr = io.Discard
	if err = cmd.Run(); err != nil {
		return Task{}, errors.New("续聊提交结果待核实；请检查 CLI 是否支持 queue，并在电脑确认消息队列，不要重复发送")
	}
	marker.Status = "queued"
	if err = c.e.State.Put(marker); err != nil {
		return Task{}, errors.New("消息已排队，但回执保存失败，请在桌面确认，不要重发")
	}
	t.Status = "queued"
	t.Body = "已排队，等待桌面处理。请在电脑打开该会话；稍后刷新查看回复。审批仍在桌面完成。"
	return t, nil
}

func codexQueueAvailable(ctx context.Context, path string, p ProviderConfig) error {
	probe, cancel := context.WithTimeout(ctx, 2*time.Second)
	defer cancel()
	cmd := codexCommand(probe, path, p, "queue", "--help")
	cmd.Stdout = io.Discard
	cmd.Stderr = io.Discard
	if cmd.Run() != nil {
		return errors.New("当前 Codex CLI 不支持续聊队列，请更新 CLI 后再试")
	}
	return nil
}

func (c *Codex) Answer(ctx context.Context, q Request) error {
	if q.Action != "queue-checked" || !validCodexThread(q.Session) {
		return errors.New("请在 Codex 桌面查看完整操作并处理审批")
	}
	if !c.sendMu.TryLock() {
		return errors.New("正在提交桌面消息，请稍后刷新")
	}
	defer c.sendMu.Unlock()
	p := c.e.Config.Snapshot().Config.Providers["codex"]
	marker, ok := c.e.State.Get("codex-queue-" + fingerprint(codexHome(p)) + "-" + q.Session)
	if !ok || (marker.Status != "queued" && marker.Status != "unknown") || q.NoticeID != fingerprint(marker.ID, marker.RequestID, marker.PreviousResponse) {
		return errors.New("队列提示已变化，请刷新后确认")
	}
	marker.Status = "history"
	marker.PreviousResponse = ""
	return c.e.State.PutIfRequest(marker, marker.RequestID)
}

func codexAction(app, title string, params map[string]any) map[string]any {
	params["provider"] = "codex"
	return map[string]any{"id": "select", "title": title, "run": "invoke", "invoke": map[string]any{"app_id": app, "params": params}}
}
func codexRow(id, title, subtitle string, action map[string]any) map[string]any {
	row := map[string]any{"type": "list_item", "id": id, "title": clip(title, 96), "subtitle": clip(subtitle, 120)}
	if action != nil {
		row["actions"] = []any{action}
	}
	return row
}

func (c *Codex) Browse(ctx context.Context, app string, q Request) map[string]any {
	if q.Workspace == "__workspaces" {
		q.Workspace = ""
	}
	r, _, _, close, err := c.connect(ctx)
	if err != nil {
		return errorEnvelope(app, err)
	}
	defer close()
	items := []any{}
	kind := "workspace"
	title := "Codex · 工作区"
	page := q.Page
	if page < 0 || page > 40 {
		return errorEnvelope(app, errors.New("无效页码"))
	}
	display := map[string]any{"card_type": "list", "title": title, "workbench_provider": "codex", "workbench_picker": true, "max_items": 8, "ttl_sec": 120}
	if q.Workspace == "" {
		// Discover recent workspace directories through the official API, never SQLite.
		paths := map[string]bool{}
		cursor := ""
		count := 0
		for n := 0; n < 2; n++ {
			doc, err := r.call("thread/list", codexListParams("", cursor, 100))
			if err != nil {
				return errorEnvelope(app, err)
			}
			for _, value := range arr(doc["data"]) {
				thread := obj(value)
				path := str(thread["cwd"])
				if path != "" {
					paths[path] = true
				}
				count++
			}
			cursor = str(doc["nextCursor"])
			if cursor == "" {
				break
			}
		}
		list := make([]string, 0, len(paths))
		for path := range paths {
			list = append(list, path)
		}
		sort.Strings(list)
		if page*5 >= len(list) {
			page = 0
		}
		end := (page + 1) * 5
		if end > len(list) {
			end = len(list)
		}
		for _, path := range list[page*5 : end] {
			ref := c.workspace(path)
			items = append(items, codexRow("__"+ref, filepath.Base(path), path, codexAction(app, "进入", map[string]any{"workspace": ref})))
		}
		for _, delta := range []int{-1, 1} {
			next := page + delta
			if next >= 0 && next*5 < len(list) {
				label := "下一页工作区"
				if delta < 0 {
					label = "上一页工作区"
				}
				items = append(items, codexRow(fmt.Sprintf("__page_%d", next), label, "", codexAction(app, label, map[string]any{"page": next})))
			}
		}
		if len(items) == 0 {
			items = append(items, codexRow("__empty", "尚无本地工作区", "请先在 Codex 桌面创建会话，再刷新", nil))
		}
		// Allow access to older conversations even when their cwd is outside the recent window.
		items = append(items, codexRow("__all", "全部会话", fmt.Sprintf("工作区来自最近 %d 条会话；更早记录在这里", count), codexAction(app, "查看", map[string]any{"workspace": "all"})))
	} else {
		kind = "session"
		cursor := ""
		previous := ""
		page = 0
		c.mu.Lock()
		path, ok := c.workspaces[q.Workspace]
		saved, pageOK := c.pages[q.Cursor]
		c.mu.Unlock()
		if q.Workspace != "all" && !ok {
			return errorEnvelope(app, errors.New("工作区选择已过期，请返回工作区重新选择"))
		}
		if q.Cursor != "" {
			if !pageOK || saved.Workspace != q.Workspace {
				return errorEnvelope(app, errors.New("分页已过期，请刷新会话列表"))
			}
			cursor = saved.Cursor
			previous = saved.Previous
			page = saved.Number
		}
		doc, err := r.call("thread/list", codexListParams(path, cursor, 5))
		if err != nil {
			return errorEnvelope(app, err)
		}
		for _, value := range arr(doc["data"]) {
			task := c.task(obj(value))
			key := sessionKey("codex", task.ID)
			items = append(items, codexRow(key, task.Title, statusLabel(task.Status)+" · "+task.Body, codexAction(app, "查看", map[string]any{"session": key})))
		}
		if page > 0 {
			items = append(items, codexRow("__previous", "上一页会话", "", codexAction(app, "上一页", map[string]any{"workspace": q.Workspace, "cursor": previous})))
		}
		if cursor = str(doc["nextCursor"]); cursor != "" {
			ref := NewID()
			c.mu.Lock()
			if len(c.pages) >= 256 {
				c.pages = map[string]codexPage{}
			}
			c.pages[ref] = codexPage{q.Workspace, cursor, q.Cursor, page + 1}
			c.mu.Unlock()
			items = append(items, codexRow("__next", "下一页会话", "", codexAction(app, "下一页", map[string]any{"workspace": q.Workspace, "cursor": ref})))
		}
		items = append(items, codexRow("__workspaces", "返回工作区", "", codexAction(app, "返回", map[string]any{})))
		display["filters"] = codexFilters(q.Workspace)
		display["title"] = "Codex · 会话"
	}
	display["workbench_picker_kind"] = kind
	display["workbench_page"] = page
	return map[string]any{"schema_version": 1, "request_id": NewID(), "app_id": app, "status": "ok", "content": items, "display": display, "spoken": map[string]any{"text": "请选择" + map[string]string{"workspace": "桌面工作区", "session": "已有会话"}[kind], "language": "zh-CN"}}
}

func codexFilters(workspace string) []any {
	if workspace == "" {
		return []any{}
	}
	return []any{map[string]any{"id": workspace, "title": "当前工作区", "workspace": workspace, "selected": true}, map[string]any{"id": "__workspaces", "title": "换工作区", "workspace": "__workspaces"}}
}
