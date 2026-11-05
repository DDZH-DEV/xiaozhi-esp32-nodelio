package bridge

import (
	"encoding/json"
	"errors"
	"os"
	"path/filepath"
	"sort"
	"sync"
	"time"
)

type State struct {
	mu    sync.Mutex
	path  string
	tasks map[string]Task
}

func OpenState(dir string) (*State, error) {
	s := &State{path: filepath.Join(dir, "sessions.json"), tasks: map[string]Task{}}
	b, err := os.ReadFile(s.path)
	if err != nil && !errors.Is(err, os.ErrNotExist) {
		return nil, err
	}
	if err == nil {
		if err = json.Unmarshal(b, &s.tasks); err != nil {
			return nil, err
		}
	}
	if s.tasks == nil {
		s.tasks = map[string]Task{}
	}
	for id, t := range s.tasks {
		if t.Status == "running" || t.Status == "waiting_approval" {
			t.Status = "unknown"
			t.Body += "\n桥已重启；上游执行结果待核实，未自动重发。"
			s.tasks[id] = t
		}
	}
	return s, writeJSON(s.path, s.tasks)
}
func (s *State) Get(id string) (Task, bool) {
	s.mu.Lock()
	defer s.mu.Unlock()
	t, ok := s.tasks[id]
	return t, ok
}
func (s *State) List(provider string) []Task {
	s.mu.Lock()
	defer s.mu.Unlock()
	out := []Task{}
	for _, t := range s.tasks {
		if t.Provider == provider {
			out = append(out, t)
		}
	}
	sort.Slice(out, func(i, j int) bool { return out[i].Updated > out[j].Updated })
	return out
}
func (s *State) Put(t Task) error {
	s.mu.Lock()
	defer s.mu.Unlock()
	return s.put(t)
}
func (s *State) put(t Task) error {
	old, ok := s.tasks[t.ID]
	if !ok && len(s.tasks) >= 1000 {
		return errors.New("session limit reached (1000); export/archive sessions before creating more")
	}
	t.Updated = time.Now().UnixMilli()
	s.tasks[t.ID] = t
	if err := writeJSON(s.path, s.tasks); err != nil {
		if ok {
			s.tasks[t.ID] = old
		} else {
			delete(s.tasks, t.ID)
		}
		return err
	}
	return nil
}

// Reserve persists before sending upstream. A repeated request ID never starts a second turn.
func (s *State) Reserve(provider, account string, r Request) (Task, bool, error) {
	s.mu.Lock()
	defer s.mu.Unlock()
	if r.RequestID != "" {
		for _, t := range s.tasks {
			if t.Provider == provider && t.RequestID == r.RequestID && t.Account == account {
				return t, false, nil
			}
		}
	}
	t := Task{ID: NewID(), Provider: provider, Title: clip(r.Instruction, 80), Account: account}
	if r.Session != "" {
		var ok bool
		t, ok = s.tasks[r.Session]
		if !ok || t.Provider != provider || t.Account != account {
			return Task{}, false, errors.New("session not found for this provider/account")
		}
		if t.Status == "running" || t.Status == "waiting_approval" || t.Status == "unknown" {
			return Task{}, false, errors.New("session busy or outcome unknown; inspect the upstream task first")
		}
	}
	t.Status = "running"
	t.Body = ""
	t.Notices = nil
	t.RequestID = r.RequestID
	if err := s.put(t); err != nil {
		return Task{}, false, err
	}
	return t, true, nil
}
