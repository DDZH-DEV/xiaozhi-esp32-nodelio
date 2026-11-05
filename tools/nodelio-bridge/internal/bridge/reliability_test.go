package bridge

import (
	"context"
	"os"
	"path/filepath"
	"strings"
	"testing"
)

func TestLedgerPersistsAcceptedAndCompletedRequests(t *testing.T) {
	dir := t.TempDir()
	l, err := OpenLedger(dir)
	if err != nil {
		t.Fatal(err)
	}
	r := Request{Provider: "openai", Instruction: "do once", RequestID: "one"}
	if _, fresh, err := l.Begin("scope:one", r); err != nil || !fresh {
		t.Fatalf("fresh=%v err=%v", fresh, err)
	}
	l, err = OpenLedger(dir)
	if err != nil {
		t.Fatal(err)
	}
	if _, fresh, err := l.Begin("scope:one", r); err == nil || fresh {
		t.Fatal("pending request may have executed; must not replay after restart")
	}
	want := []Task{{ID: sessionKey("openai", "one"), Status: "running"}}
	if err := l.Finish("scope:one", want, nil); err != nil {
		t.Fatal(err)
	}
	l, err = OpenLedger(dir)
	if err != nil {
		t.Fatal(err)
	}
	got, fresh, err := l.Begin("scope:one", r)
	if err != nil || fresh || len(got.Tasks) != 1 || got.Tasks[0].ID != want[0].ID {
		t.Fatalf("receipt=%+v fresh=%v err=%v", got, fresh, err)
	}
	r.Instruction = "different command"
	if _, _, err := l.Begin("scope:one", r); err == nil {
		t.Fatal("request ID accepted for different parameters")
	}
}

func TestLedgerDoesNotEvictUncertainMutations(t *testing.T) {
	l, err := OpenLedger(t.TempDir())
	if err != nil {
		t.Fatal(err)
	}
	for i := 0; i < 4096; i++ {
		l.entries[string(rune(i))] = receipt{Pending: true, Created: 1}
	}
	if _, fresh, err := l.Begin("new", Request{Instruction: "new"}); err == nil || fresh {
		t.Fatal("uncertain request evicted to make room")
	}
}

func TestLegacyImportIsExplicitAndPreservesOriginal(t *testing.T) {
	e, _ := fixture(t)
	path := filepath.Join(t.TempDir(), "bridge-config.json")
	original := []byte(`{"host_ws":"wss://host.example/ws/dsh-bridge","bridge_token":"legacy-test-token","default_model":"model","default_permission":"limited","default_agent_preset":"standard"}`)
	if err := os.WriteFile(path, original, 0600); err != nil {
		t.Fatal(err)
	}
	if err := e.Config.ImportLegacy(path); err != nil {
		t.Fatal(err)
	}
	s := e.Config.Snapshot()
	if !s.Config.Providers["dsh"].Enabled || s.Config.Providers["openai"].Enabled || s.Secrets["host_token"] != "legacy-test-token" || s.Config.Providers["dsh"].Permission != "limited" {
		t.Fatalf("legacy fields not imported")
	}
	if err := e.Config.ImportLegacy(path); err == nil {
		t.Fatal("overwrote enabled connector")
	}
	after, err := os.ReadFile(path)
	if err != nil || string(after) != string(original) {
		t.Fatal("legacy file changed")
	}
}

func TestHostNeverAnnouncesUncertainTaskAsCompleted(t *testing.T) {
	e, _ := fixture(t)
	h := NewHost(e)
	h.enqueue(Task{ID: "local", Provider: "workbuddy", Status: "submitted"})
	if len(h.outbox) != 0 {
		t.Fatal("submission emitted a completion notification")
	}
	h.enqueue(Task{ID: "uncertain", Provider: "openai", Status: "unknown", Title: "task"})
	if len(h.outbox) != 1 || h.outbox[0]["reason"] == "completed" || h.outbox[0]["mode"] != "info" || !strings.Contains(h.outbox[0]["title"].(string), "结果待核实") {
		t.Fatalf("misleading notification: %+v", h.outbox)
	}
}

func TestWorkBuddyApprovalRequiresCurrentNoticeAndOriginalChoice(t *testing.T) {
	e, _ := fixture(t)
	configure(t, e, "workbuddy", "")
	s := e.Config.Snapshot()
	c := s.Config.Providers["workbuddy"]
	c.Mode = "cloud"
	s.Config.Providers["workbuddy"] = c
	if err := e.Config.Update(ConfigUpdate{Config: s.Config}); err != nil {
		t.Fatal(err)
	}
	p := e.Providers["workbuddy"].(*WorkBuddy)
	task := Task{ID: "approval", Provider: "workbuddy", Status: "waiting_approval", Account: fingerprint(strings.TrimRight(c.BaseURL, "/"), s.Secrets["workbuddy_token"], c.Mode)}
	if err := e.State.Put(task); err != nil {
		t.Fatal(err)
	}
	pending := &pendingPermission{notice: Notice{ID: "current", Options: []Option{{ID: "deny-once"}, {ID: "allow-once"}}}, answer: make(chan string, 1)}
	p.pending[task.ID] = pending
	for _, r := range []Request{{Session: task.ID, NoticeID: "old", Action: "allow-once"}, {Session: task.ID, NoticeID: "current", Action: "allow-always"}} {
		if p.Answer(context.Background(), r) == nil {
			t.Fatal("stale or invented approval accepted")
		}
	}
	if len(pending.answer) != 0 {
		t.Fatal("permission was answered without a valid choice")
	}
	r := Request{Session: task.ID, NoticeID: "current", Action: "deny-once"}
	if err := p.Answer(context.Background(), r); err != nil {
		t.Fatal(err)
	}
	if got := <-pending.answer; got != "deny-once" {
		t.Fatalf("choice changed: %q", got)
	}
	if p.Answer(context.Background(), r) == nil {
		t.Fatal("approval accepted twice")
	}
}
