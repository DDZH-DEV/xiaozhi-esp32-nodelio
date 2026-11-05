package bridge

import (
	"context"
	"encoding/json"
	"fmt"
	"net/http"
	"net/http/httptest"
	"strings"
	"sync/atomic"
	"testing"
	"time"

	"github.com/gorilla/websocket"
)

type workbenchFixture struct {
	task    Task
	sends   atomic.Int32
	answers atomic.Int32
}

func (p *workbenchFixture) Health(context.Context) Health {
	return Health{State: "ready", Message: "fixture"}
}
func (p *workbenchFixture) List(context.Context, Request) ([]Task, error) { return []Task{p.task}, nil }
func (p *workbenchFixture) Send(context.Context, Request) (Task, error) {
	p.sends.Add(1)
	return p.task, nil
}
func (p *workbenchFixture) Answer(context.Context, Request) error { p.answers.Add(1); return nil }

func TestWorkbenchSingleAppActionsAndDetail(t *testing.T) {
	e, _ := fixture(t)
	configure(t, e, "workbuddy", "")
	p := &workbenchFixture{task: Task{ID: "one", Provider: "workbuddy", Status: "completed", Title: "task", Body: "answer"}}
	e.Providers["workbuddy"] = p
	ctx := context.Background()
	doc := e.Invoke(ctx, "workbuddy.status", Request{})
	rows := arr(doc["content"])
	if len(rows) != 2 || obj(rows[0])["body"] != nil || obj(rows[1])["id"] != "__new__" {
		t.Fatalf("list accidentally became detail or lost new-task action: %+v", doc)
	}
	key := str(obj(rows[0])["id"])
	for _, row := range rows {
		for _, a := range arr(obj(row)["actions"]) {
			if obj(obj(a)["invoke"])["app_id"] != "workbuddy.status" {
				t.Fatal("action needs separately installed helper")
			}
		}
	}
	detail := e.Invoke(ctx, "workbuddy.status", Request{Session: key})
	if obj(arr(detail["content"])[0])["body"] != "answer" {
		t.Fatal("detail missing body")
	}
	r := Request{Operation: "task", Instruction: "do once", RequestID: "logical-one"}
	for i := 0; i < 2; i++ {
		if e.Invoke(ctx, "workbuddy.status", r)["status"] != "ok" {
			t.Fatal("status app cannot submit")
		}
	}
	if p.sends.Load() != 1 {
		t.Fatal("logical task replayed")
	}
	if e.Invoke(ctx, "workbuddy.status", Request{Arm: true})["status"] != "error" {
		t.Fatal("local arm sent upstream")
	}
	if e.Invoke(ctx, "workbuddy.status", Request{Session: sessionKey("openai", "one")})["status"] != "error" {
		t.Fatal("cross-platform session accepted")
	}
}

func TestWorkbenchApprovalUsesStableKeyAndMainApp(t *testing.T) {
	e, _ := fixture(t)
	configure(t, e, "workbuddy", "")
	p := &workbenchFixture{task: Task{ID: "one", Provider: "workbuddy", Status: "waiting_approval", Notices: []Notice{{ID: "n1", Title: "Allow?", Options: []Option{{ID: "allow-once", Title: "Allow once"}}}}}}
	e.Providers["workbuddy"] = p
	ctx := context.Background()
	getAction := func() (map[string]any, Request) {
		doc := e.Invoke(ctx, "workbuddy.status", Request{Session: sessionKey("workbuddy", "one")})
		inv := obj(obj(arr(obj(arr(doc["content"])[0])["actions"])[0])["invoke"])
		b, _ := json.Marshal(inv["params"])
		var r Request
		if err := json.Unmarshal(b, &r); err != nil {
			t.Fatal(err)
		}
		return inv, r
	}
	inv, r := getAction()
	_, again := getAction()
	if inv["app_id"] != "workbuddy.status" || r.Operation != "answer" || r.RequestID == "" || r.RequestID != again.RequestID {
		t.Fatal("approval lost routing or stable ID")
	}
	for i := 0; i < 2; i++ {
		if e.Invoke(ctx, "workbuddy.status", r)["status"] != "ok" {
			t.Fatal("approval failed")
		}
	}
	if p.answers.Load() != 1 {
		t.Fatal("approval replayed")
	}
}

func TestMarketAdvertisesMainAppsAndLiveAvailability(t *testing.T) {
	e, _ := fixture(t)
	configure(t, e, "openai", "")
	e.Providers["openai"] = &workbenchFixture{}
	s := e.Config.Snapshot()
	s.Config.DefaultProvider = "openai"
	if err := e.Config.Update(ConfigUpdate{Config: s.Config}); err != nil {
		t.Fatal(err)
	}
	var doc map[string]any
	if err := json.Unmarshal(e.MarketIndex(context.Background()), &doc); err != nil {
		t.Fatal(err)
	}
	apps := arr(doc["apps"])
	if len(apps) != 4 {
		t.Fatalf("got %d visible apps", len(apps))
	}
	for _, raw := range apps {
		a := obj(raw)
		id := str(a["app_id"])
		if !strings.HasSuffix(id, ".status") {
			t.Fatal("internal helper shown as standalone install")
		}
		want := "disabled"
		if id == "openai.status" || id == "workbench.status" {
			want = "ready"
		}
		if obj(a["availability"])["state"] != want {
			t.Fatalf("%s: %+v", id, a["availability"])
		}
	}
}

func TestHostKeepsLogicalRequestAcrossTransportRetries(t *testing.T) {
	e, cancel := fixture(t)
	configure(t, e, "openai", "")
	p := &workbenchFixture{task: Task{ID: "one", Provider: "openai", Status: "running"}}
	e.Providers["openai"] = p
	done := make(chan error, 1)
	upgrader := websocket.Upgrader{}
	upstream := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		c, err := upgrader.Upgrade(w, r, nil)
		if err != nil {
			done <- err
			return
		}
		defer c.Close()
		c.SetReadDeadline(time.Now().Add(5 * time.Second))
		var reg map[string]any
		if err = c.ReadJSON(&reg); err != nil {
			done <- err
			return
		}
		if obj(obj(reg["providers"])["openai"])["state"] != "ready" {
			done <- fmt.Errorf("registration lacks provider status")
			return
		}
		_ = c.WriteJSON(map[string]any{"type": "registered"})
		for _, transport := range []string{"wire-1", "wire-2"} {
			_ = c.WriteJSON(map[string]any{"type": "invoke", "request_id": transport, "app_id": "openai.status", "params": Request{Operation: "task", Instruction: "once", RequestID: "logical"}})
			var result map[string]any
			if err = c.ReadJSON(&result); err != nil {
				done <- err
				return
			}
			if result["request_id"] != transport || obj(result["body"])["status"] != "ok" {
				done <- fmt.Errorf("bad transport reply: %+v", result)
				return
			}
		}
		done <- nil
		<-e.ctx.Done()
	}))
	defer upstream.Close()
	s := e.Config.Snapshot()
	s.Config.HostWS = "ws" + strings.TrimPrefix(upstream.URL, "http")
	if err := e.Config.Update(ConfigUpdate{Config: s.Config, Secrets: map[string]string{"host_token": "fixture"}}); err != nil {
		t.Fatal(err)
	}
	h := NewHost(e)
	hostDone := make(chan struct{})
	go func() { defer close(hostDone); h.Run(e.ctx) }()
	select {
	case err := <-done:
		if err != nil {
			t.Error(err)
		}
	case <-time.After(8 * time.Second):
		t.Error("Host timeout")
	}
	cancel()
	<-hostDone
	if p.sends.Load() != 1 {
		t.Fatalf("transport retry sent upstream %d times", p.sends.Load())
	}
}
