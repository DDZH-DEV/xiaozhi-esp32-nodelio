package bridge

import (
	"bufio"
	"bytes"
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"net/http"
	"strings"
	"time"
)

type acpFrame struct {
	ID     json.RawMessage `json:"id"`
	Method string          `json:"method"`
	Params map[string]any  `json:"params"`
	Result map[string]any  `json:"result"`
	Error  json.RawMessage `json:"error"`
}

// SSE events may span multiple data lines; a bounded scanner prevents unbounded frames.
func readSSE(ctx context.Context, r io.Reader, out chan<- acpFrame) error {
	s := bufio.NewScanner(r)
	s.Buffer(make([]byte, 4096), 1<<20)
	var data strings.Builder
	flush := func() error {
		if data.Len() == 0 {
			return nil
		}
		var f acpFrame
		err := json.Unmarshal([]byte(data.String()), &f)
		data.Reset()
		if err != nil {
			return errors.New("invalid ACP event")
		}
		select {
		case out <- f:
			return nil
		case <-ctx.Done():
			return ctx.Err()
		}
	}
	for s.Scan() {
		line := s.Text()
		if line == "" {
			if err := flush(); err != nil {
				return err
			}
			continue
		}
		if strings.HasPrefix(line, "data:") {
			value := strings.TrimPrefix(line, "data:")
			value = strings.TrimPrefix(value, " ")
			if data.Len() > 0 {
				data.WriteByte('\n')
			}
			data.WriteString(value)
			if data.Len() > 1<<20 {
				return errors.New("ACP event too large")
			}
		}
	}
	if err := s.Err(); err != nil {
		return err
	}
	if err := flush(); err != nil {
		return err
	}
	return io.EOF
}

func (p *WorkBuddy) runACP(ctx context.Context, t *Task, link, token, instruction string) error {
	if token == "" {
		return errors.New("WorkBuddy task connection is not ready")
	}
	if err := validateURL(link, false); err != nil {
		return fmt.Errorf("invalid ACP link: %w", err)
	}
	ctx, cancel := context.WithCancel(ctx)
	defer cancel()
	req, err := http.NewRequestWithContext(ctx, "GET", link, nil)
	if err != nil {
		return err
	}
	req.Header.Set("Authorization", "Bearer "+token)
	req.Header.Set("Accept", "text/event-stream")
	streamClient := &http.Client{Transport: &http.Transport{ResponseHeaderTimeout: 15 * time.Second}, CheckRedirect: func(*http.Request, []*http.Request) error { return http.ErrUseLastResponse }}
	defer streamClient.CloseIdleConnections()
	resp, err := streamClient.Do(req)
	if err != nil {
		return errors.New("ACP stream connection failed")
	}
	defer resp.Body.Close()
	connection := resp.Header.Get("Acp-Connection-Id")
	if resp.StatusCode != 200 || connection == "" {
		return fmt.Errorf("ACP stream rejected (HTTP %d)", resp.StatusCode)
	}
	frames := make(chan acpFrame, 32)
	readErr := make(chan error, 1)
	go func() { readErr <- readSSE(ctx, resp.Body, frames) }()
	post := func(body any) error {
		b, err := json.Marshal(body)
		if err != nil {
			return err
		}
		r, err := http.NewRequestWithContext(ctx, "POST", link, bytes.NewReader(b))
		if err != nil {
			return err
		}
		r.Header.Set("Authorization", "Bearer "+token)
		r.Header.Set("Content-Type", "application/json")
		r.Header.Set("Acp-Connection-Id", connection)
		res, err := p.http.Do(r)
		if err != nil {
			return errors.New("ACP send outcome unknown")
		}
		defer res.Body.Close()
		_, _ = io.Copy(io.Discard, io.LimitReader(res.Body, 64<<10))
		if res.StatusCode < 200 || res.StatusCode >= 300 {
			return fmt.Errorf("ACP HTTP %d", res.StatusCode)
		}
		return nil
	}
	lastSave := time.Now()
	handle := func(f acpFrame) error {
		if f.Method == "session/update" {
			u := obj(f.Params["update"])
			if str(u["sessionUpdate"]) == "agent_message_chunk" {
				t.Body = clip(t.Body+str(obj(u["content"])["text"]), 16000)
			}
			if time.Since(lastSave) > 2*time.Second {
				if err := p.e.State.Put(*t); err != nil {
					return err
				}
				lastSave = time.Now()
			}
			return nil
		}
		if f.Method == "session/request_permission" && len(f.ID) > 0 {
			n := Notice{ID: string(f.ID), Title: str(obj(f.Params["toolCall"])["title"]), Options: []Option{}}
			for _, o := range arr(f.Params["options"]) {
				m := obj(o)
				if id := str(m["optionId"]); id != "" {
					n.Options = append(n.Options, Option{ID: id, Title: str(m["name"])})
				}
			}
			if len(n.Options) == 0 {
				return errors.New("ACP question requires desktop input; no supported options")
			}
			pending := &pendingPermission{notice: n, answer: make(chan string, 1)}
			p.mu.Lock()
			p.pending[t.ID] = pending
			p.mu.Unlock()
			t.Status = "waiting_approval"
			t.Notices = []Notice{n}
			p.e.finish(*t)
			select {
			case choice := <-pending.answer:
				if err := post(map[string]any{"jsonrpc": "2.0", "id": f.ID, "result": map[string]any{"outcome": map[string]any{"outcome": "selected", "optionId": choice}}}); err != nil {
					return err
				}
			case <-ctx.Done():
				return ctx.Err()
			}
			p.mu.Lock()
			delete(p.pending, t.ID)
			p.mu.Unlock()
			t.Status = "running"
			t.Notices = nil
			return p.e.State.Put(*t)
		}
		// Never leave an unsupported server-to-client request hanging silently.
		if f.Method != "" && len(f.ID) > 0 {
			return post(map[string]any{"jsonrpc": "2.0", "id": f.ID, "error": map[string]any{"code": -32601, "message": "Client capability not supported"}})
		}
		return nil
	}
	steps := []struct {
		method string
		params any
	}{
		{"initialize", map[string]any{"protocolVersion": 1, "clientCapabilities": map[string]any{"fs": map[string]bool{"readTextFile": false, "writeTextFile": false}}}},
		{"session/load", map[string]any{"sessionId": t.RemoteID, "cwd": "/workspace", "mcpServers": []any{}}},
		{"session/prompt", map[string]any{"sessionId": t.RemoteID, "prompt": []any{map[string]any{"type": "text", "text": instruction}}}},
	}
	for i, step := range steps {
		id := i + 1
		if err = post(map[string]any{"jsonrpc": "2.0", "id": id, "method": step.method, "params": step.params}); err != nil {
			return err
		}
		for {
			var f acpFrame
			select {
			case f = <-frames:
			case e := <-readErr:
				// Reader can reach EOF immediately after buffering the final response.
				select {
				case f = <-frames:
					readErr <- e
				default:
					return errors.New("ACP stream closed before turn completion")
				}
			case <-ctx.Done():
				return ctx.Err()
			}
			if string(f.ID) == fmt.Sprint(id) && f.Method == "" {
				if len(f.Error) > 0 && string(f.Error) != "null" {
					return fmt.Errorf("ACP %s failed", step.method)
				}
				if step.method == "session/prompt" {
					switch str(f.Result["stopReason"]) {
					case "end_turn":
						t.Status = "completed"
					case "cancelled":
						t.Status = "cancelled"
					case "refusal":
						t.Status = "failed"
					default:
						t.Status = "incomplete"
					}
				}
				break
			}
			if err = handle(f); err != nil {
				return err
			}
		}
	}
	return nil
}
