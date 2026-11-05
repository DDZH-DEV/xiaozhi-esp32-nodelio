package bridge

import (
	"context"
	"errors"
	"strings"
)

func (e *Engine) Invoke(ctx context.Context, app string, r Request) map[string]any {
	parts := strings.Split(app, ".")
	if len(parts) != 2 {
		return errorEnvelope(app, errors.New("unknown skill"))
	}
	id, op := parts[0], parts[1]
	if id != "workbench" && id != "dsh" && id != "openai" && id != "workbuddy" {
		return errorEnvelope(app, errors.New("unknown provider"))
	}
	if id != "workbench" {
		if r.Provider != "" && r.Provider != id {
			return errorEnvelope(app, errors.New("provider mismatch"))
		}
		r.Provider = id
	}
	// Existing DSH cards contain raw external session IDs. Only this legacy skill accepts them.
	legacy := id == "dsh"
	if legacy && r.Session != "" && !strings.HasPrefix(r.Session, "dsh:") {
		r.Session = sessionKey("dsh", r.Session)
	}
	tasks, err := e.Dispatch(ctx, op, r)
	if err != nil {
		return errorEnvelope(app, err)
	}
	items := []any{}
	for _, t := range tasks {
		key := t.ID
		if legacy {
			_, raw, err := parseSession(key)
			if err == nil {
				key = raw
			}
		}
		body := t.Body
		var questions []string
		for _, notice := range t.Notices {
			if notice.Title != "" {
				questions = append(questions, clip(notice.Title, 1500))
			}
		}
		if len(questions) > 0 {
			body = "待处理：" + strings.Join(questions, "\n") + "\n\n" + body
		}
		row := map[string]any{"type": "list_item", "id": key, "title": clip(t.Title, 96), "subtitle": clip(statusLabel(t.Status)+" · "+body, 120), "body": clip(body, 6000)}
		actions := []any{}
		if r.Session == "" {
			actions = append(actions, map[string]any{"id": "detail", "title": "查看", "run": "invoke", "invoke": map[string]any{"app_id": id + ".status", "params": map[string]any{"provider": t.Provider, "session": key}}})
		}
		for _, n := range t.Notices {
			for _, opt := range n.Options {
				if len(actions) >= 4 {
					break
				}
				actions = append(actions, map[string]any{"id": opt.ID, "title": opt.Title, "run": "invoke", "invoke": map[string]any{"app_id": id + ".answer", "params": map[string]any{"provider": t.Provider, "session": key, "notice_id": n.ID, "action": opt.ID}}})
			}
		}
		if len(actions) > 0 {
			row["actions"] = actions
		}
		items = append(items, row)
	}
	return map[string]any{"schema_version": 1, "request_id": NewID(), "app_id": app, "status": "ok", "content": items,
		"display": map[string]any{"card_type": "list", "title": "NodeLio 工作台", "preferred_surface": "card", "max_items": 8, "ttl_sec": 120},
		"spoken":  map[string]any{"text": map[bool]string{true: "指令已提交，可查看任务进度。", false: "任务列表已更新。"}[op == "task"], "language": "zh-CN"}}
}
func statusLabel(s string) string {
	if label, ok := map[string]string{"running": "执行中", "completed": "已完成", "idle": "空闲", "waiting": "等你确认", "waiting_approval": "等你确认", "pending": "等你确认", "unknown": "结果待核实", "failed": "失败", "submitted": "已投递", "history": "消息历史", "cancelled": "已取消", "incomplete": "未完整结束"}[s]; ok {
		return label
	}
	return s
}
func errorEnvelope(app string, err error) map[string]any {
	return map[string]any{"schema_version": 1, "request_id": NewID(), "app_id": app, "status": "error", "content": []any{}, "display": map[string]any{"card_type": "error", "title": "连接桥提示"}, "error": map[string]any{"code": "upstream_error", "message": clip(err.Error(), 300), "retryable": false}, "spoken": map[string]any{"text": clip(err.Error(), 180), "language": "zh-CN"}}
}
