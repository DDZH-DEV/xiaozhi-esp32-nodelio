package bridge

import (
	"context"
	"crypto/sha256"
	"encoding/hex"
	"errors"
	"fmt"
	"log/slog"
	"strings"
	"sync"
	"time"
)

type Engine struct {
	Config    *ConfigStore
	State     *State
	Providers map[string]Provider
	ctx       context.Context
	jobs      chan struct{}
	wg        sync.WaitGroup
	Notice    chan Task
	Ledger    *Ledger
}

func NewEngine(ctx context.Context, c *ConfigStore, s *State) *Engine {
	e := &Engine{Config: c, State: s, ctx: ctx, jobs: make(chan struct{}, 8), Notice: make(chan Task, 64)}
	e.Providers = map[string]Provider{"dsh": NewDSH(c), "openai": &OpenAI{e: e, http: newHTTPClient()}, "workbuddy": NewWorkBuddy(e)}
	return e
}
func fingerprint(parts ...string) string {
	h := sha256.Sum256([]byte(strings.Join(parts, "\x00")))
	return hex.EncodeToString(h[:12])
}
func (e *Engine) run(fn func(context.Context)) bool {
	select {
	case e.jobs <- struct{}{}:
	default:
		return false
	}
	e.wg.Add(1)
	go func() {
		defer e.wg.Done()
		defer func() { <-e.jobs }()
		ctx, cancel := context.WithTimeout(e.ctx, 10*time.Minute)
		defer cancel()
		fn(ctx)
	}()
	return true
}
func (e *Engine) finish(t Task) {
	if err := e.State.Put(t); err != nil {
		slog.Error("persist task result", "error", err)
		return
	}
	select {
	case e.Notice <- t:
	default:
		slog.Warn("notification buffer full; result remains in session history")
	}
}
func (e *Engine) Wait() { e.wg.Wait() }
func (e *Engine) Dispatch(ctx context.Context, operation string, r Request) ([]Task, error) {
	snapshot := e.Config.Snapshot()
	c := snapshot.Config
	id := r.Provider
	if r.Session != "" {
		provider, raw, err := parseSession(r.Session)
		if err != nil {
			return nil, err
		}
		if id != "" && id != provider {
			return nil, errors.New("session belongs to another provider")
		}
		id = provider
		r.Session = raw
	}
	if id == "" {
		id = c.DefaultProvider
	}
	p, ok := e.Providers[id]
	if !ok {
		return nil, errors.New("unknown provider")
	}
	if !c.Providers[id].Enabled {
		return nil, fmt.Errorf("%s is disabled", id)
	}
	r.Provider = id
	r.Instruction = strings.TrimSpace(r.Instruction)
	if r.Workspace == "" {
		r.Workspace = r.DefaultWorkspace
	}
	if operation != "status" && operation != "task" && operation != "answer" {
		return nil, errors.New("unsupported operation")
	}
	if operation == "task" && (len(r.Instruction) == 0 || len(r.Instruction) > 8000) {
		return nil, errors.New("instruction must contain 1–8000 UTF-8 bytes")
	}
	if r.NewSession && r.Session != "" {
		return nil, errors.New("cannot combine new_session and an existing session")
	}
	if operation == "answer" && (r.Session == "" || r.NoticeID == "" || r.Action == "") {
		return nil, errors.New("session, notice_id and action are required")
	}
	ledgerKey := ""
	if operation != "status" && r.RequestID != "" && e.Ledger != nil {
		identity := fingerprint(c.Providers[id].BaseURL, c.Providers[id].EndpointFile, c.Providers[id].Mode, snapshot.Secrets[map[string]string{"openai": "openai_key", "workbuddy": "workbuddy_token"}[id]])
		ledgerKey = id + ":" + identity + ":" + operation + ":" + r.RequestID
		receipt, fresh, err := e.Ledger.Begin(ledgerKey, r)
		if err != nil {
			return nil, err
		}
		if !fresh {
			if receipt.Error != "" {
				return nil, errors.New(receipt.Error)
			}
			return receipt.Tasks, nil
		}
	}
	var tasks []Task
	var err error
	switch operation {
	case "status":
		tasks, err = p.List(ctx, r)
	case "task":
		var t Task
		t, err = p.Send(ctx, r)
		if err == nil {
			tasks = []Task{t}
		}
	case "answer":
		err = p.Answer(ctx, r)
		if err == nil {
			tasks, err = p.List(ctx, r)
		}
	default:
		return nil, errors.New("unsupported operation")
	}
	if len(tasks) > 8 {
		tasks = tasks[:8]
	}
	for i := range tasks {
		tasks[i].ID = sessionKey(id, tasks[i].ID)
		tasks[i].RemoteID = ""
		tasks[i].PreviousResponse = ""
		tasks[i].Account = ""
		tasks[i].RequestID = ""
		tasks[i].Notices = append([]Notice(nil), tasks[i].Notices...)
		for j := range tasks[i].Notices {
			tasks[i].Notices[j].Raw = nil
		}
	}
	if ledgerKey != "" {
		if saveErr := e.Ledger.Finish(ledgerKey, tasks, err); saveErr != nil {
			return nil, fmt.Errorf("request outcome could not be persisted; do not resend: %w", saveErr)
		}
	}
	return tasks, err
}
func (e *Engine) Health(ctx context.Context) map[string]Health {
	result := map[string]Health{}
	var mu sync.Mutex
	var wg sync.WaitGroup
	c := e.Config.Snapshot().Config
	for id, p := range e.Providers {
		wg.Add(1)
		go func(id string, p Provider) {
			defer wg.Done()
			h := Health{State: "disabled", Message: "未启用", Capabilities: []string{}}
			if c.Providers[id].Enabled {
				probe, cancel := context.WithTimeout(ctx, 5*time.Second)
				defer cancel()
				h = p.Health(probe)
			}
			mu.Lock()
			result[id] = h
			mu.Unlock()
		}(id, p)
	}
	wg.Wait()
	return result
}
