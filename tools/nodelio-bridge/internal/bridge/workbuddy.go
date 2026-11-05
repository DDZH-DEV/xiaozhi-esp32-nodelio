package bridge

import (
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"net/http"
	"net/url"
	"strings"
	"sync"
	"time"
)

type pendingPermission struct {
	notice Notice
	answer chan string
	used   bool
}
type WorkBuddy struct {
	e         *Engine
	http      *http.Client
	mu        sync.Mutex
	pending   map[string]*pendingPermission
	localGate chan struct{}
}

func NewWorkBuddy(e *Engine) *WorkBuddy {
	return &WorkBuddy{e: e, http: newHTTPClient(), pending: map[string]*pendingPermission{}, localGate: make(chan struct{}, 1)}
}
func wbCall(ctx context.Context, c *http.Client, method, endpoint, token string, body any) (map[string]any, error) {
	var doc map[string]any
	if err := callJSON(ctx, c, method, endpoint, token, body, &doc); err != nil {
		return nil, err
	}
	if code, ok := doc["code"].(float64); ok && code != 0 {
		return nil, fmt.Errorf("WorkBuddy API error %g (check authorization and scopes)", code)
	}
	if data, ok := doc["data"].(map[string]any); ok {
		return data, nil
	}
	return doc, nil
}
func (p *WorkBuddy) Health(ctx context.Context) Health {
	s := p.e.Config.Snapshot()
	cfg := s.Config.Providers["workbuddy"]
	token := s.Secrets["workbuddy_token"]
	h := Health{State: "needs_auth", Message: "填写开放平台已授权的 Access Token", Capabilities: []string{"list", "create", "continue"}}
	if token == "" {
		return h
	}
	endpoint := "/localassistant"
	if cfg.Mode == "cloud" {
		endpoint = "/tasks?page=1&size=1"
		h.Capabilities = append(h.Capabilities, "approval")
	}
	doc, err := wbCall(ctx, p.http, "GET", strings.TrimRight(cfg.BaseURL, "/")+endpoint, token, nil)
	if err != nil {
		h.State = "error"
		h.Message = err.Error()
		return h
	}
	h.State = "ready"
	h.Message = "WorkBuddy 已连接"
	if cfg.Mode == "local" && doc["online"] != true {
		h.State = "offline"
		h.Message = "WorkBuddy 本地助理未在线"
	}
	return h
}
func (p *WorkBuddy) List(ctx context.Context, r Request) ([]Task, error) {
	s := p.e.Config.Snapshot()
	cfg := s.Config.Providers["workbuddy"]
	token := s.Secrets["workbuddy_token"]
	account := fingerprint(strings.TrimRight(cfg.BaseURL, "/"), token, cfg.Mode)
	if token == "" {
		return nil, errors.New("WorkBuddy authorization required")
	}
	if cfg.Mode == "local" {
		id := "local-" + account
		if r.Session != "" && r.Session != id {
			return nil, errors.New("session belongs to another WorkBuddy mode/account")
		}
		doc, err := wbCall(ctx, p.http, "GET", strings.TrimRight(cfg.BaseURL, "/")+"/localassistant/message?limit=20&offset=0", token, nil)
		if err != nil {
			return nil, err
		}
		lines := []string{}
		for _, message := range arr(doc["messages"]) {
			m := obj(message)
			lines = append(lines, str(m["role"])+": "+textParts(m["content"]))
		}
		return []Task{{ID: id, Provider: "workbuddy", Title: "WorkBuddy 本地助理", Status: "history", Body: clip(strings.Join(lines, "\n\n"), 16000)}}, nil
	}
	out := []Task{}
	for _, t := range p.e.State.List("workbuddy") {
		if t.Account == account && (r.Session == "" || r.Session == t.ID) {
			out = append(out, t)
		}
	}
	return out, nil
}
func (p *WorkBuddy) Send(ctx context.Context, r Request) (Task, error) {
	s := p.e.Config.Snapshot()
	cfg := s.Config.Providers["workbuddy"]
	token := s.Secrets["workbuddy_token"]
	base := strings.TrimRight(cfg.BaseURL, "/")
	account := fingerprint(base, token, cfg.Mode)
	if token == "" {
		return Task{}, errors.New("WorkBuddy authorization required")
	}
	if cfg.Mode == "local" {
		select {
		case p.localGate <- struct{}{}:
		case <-ctx.Done():
			return Task{}, ctx.Err()
		}
		defer func() { <-p.localGate }()
		id := "local-" + account
		if r.Session != "" && r.Session != id {
			return Task{}, errors.New("session belongs to another WorkBuddy mode/account")
		}
		if r.NewSession {
			return Task{}, errors.New("WorkBuddy local API exposes one message stream, not independent sessions")
		}
		old, ok := p.e.State.Get(id)
		if ok && r.RequestID != "" && old.RequestID == r.RequestID {
			return old, nil
		}
		t := Task{ID: id, Provider: "workbuddy", Title: "WorkBuddy 本地助理", Status: "unknown", Account: account, RequestID: r.RequestID, Body: "消息正在投递；若连接中断请先查看历史，勿自动重发。"}
		if err := p.e.State.Put(t); err != nil {
			return t, err
		}
		doc, err := wbCall(ctx, p.http, "POST", base+"/localassistant/message", token, map[string]any{"content": r.Instruction, "msg_type": "text"})
		if err != nil {
			t.Body = err.Error()
			p.e.finish(t)
			return t, err
		}
		t.Status = "submitted"
		t.RemoteID = str(doc["message_id"])
		t.Body = "已投递给本地助理。刷新查看消息历史；已投递不代表执行完成。"
		p.e.finish(t)
		return t, nil
	}
	t, fresh, err := p.e.State.Reserve("workbuddy", account, r)
	if err != nil || !fresh {
		return t, err
	}
	if !p.e.run(func(jobCtx context.Context) {
		t := t
		var doc map[string]any
		var err error
		if t.RemoteID == "" {
			doc, err = wbCall(jobCtx, p.http, "POST", base+"/tasks", token, map[string]any{"prompt": r.Instruction, "name": t.Title})
			if err == nil {
				t.RemoteID = str(doc["task_id"])
				if t.RemoteID == "" {
					err = errors.New("WorkBuddy did not return task_id")
				}
			}
			if err == nil {
				err = p.e.State.Put(t)
			}
		}
		if err == nil && (doc == nil || str(doc["link"]) == "" || str(doc["token"]) == "") {
			// Read-only preparation may be retried; task creation and prompts never are.
			for i := 0; i < 5; i++ {
				doc, err = wbCall(jobCtx, p.http, "GET", base+"/tasks/"+url.PathEscape(t.RemoteID), token, nil)
				if err != nil || str(doc["link"]) != "" && str(doc["token"]) != "" {
					break
				}
				select {
				case <-time.After(time.Second):
				case <-jobCtx.Done():
					err = jobCtx.Err()
				}
			}
		}
		if err == nil {
			err = p.runACP(jobCtx, &t, str(doc["link"]), str(doc["token"]), r.Instruction)
		}
		if err != nil {
			t.Status = "unknown"
			t.Body = clip(t.Body+"\n"+err.Error()+"；未自动重发，请在 WorkBuddy 核实。", 16000)
		}
		p.mu.Lock()
		delete(p.pending, t.ID)
		p.mu.Unlock()
		t.Notices = nil
		p.e.finish(t)
	}) {
		t.Status = "failed"
		t.Body = "桥任务队列已满，未发送"
		p.e.finish(t)
		return t, errors.New(t.Body)
	}
	return t, nil
}
func (p *WorkBuddy) Answer(_ context.Context, r Request) error {
	s := p.e.Config.Snapshot()
	cfg := s.Config.Providers["workbuddy"]
	if cfg.Mode != "cloud" {
		return errors.New("answer local assistant questions in WorkBuddy desktop")
	}
	t, ok := p.e.State.Get(r.Session)
	if !ok || t.Account != fingerprint(strings.TrimRight(cfg.BaseURL, "/"), s.Secrets["workbuddy_token"], cfg.Mode) {
		return errors.New("session account mismatch")
	}
	p.mu.Lock()
	defer p.mu.Unlock()
	pending, ok := p.pending[r.Session]
	if !ok || pending.used || pending.notice.ID != r.NoticeID {
		return errors.New("permission request expired or already submitted")
	}
	allowed := false
	for _, opt := range pending.notice.Options {
		if opt.ID == r.Action {
			allowed = true
		}
	}
	if !allowed {
		return errors.New("action is not a choice in this permission request")
	}
	pending.used = true
	pending.answer <- r.Action
	return nil
}

func marshalRaw(v any) json.RawMessage { b, _ := json.Marshal(v); return b }
