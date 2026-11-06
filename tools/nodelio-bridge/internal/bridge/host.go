package bridge

import (
	"context"
	"encoding/json"
	"fmt"
	"github.com/gorilla/websocket"
	"net/http"
	"runtime"
	"strings"
	"sync"
	"sync/atomic"
	"time"
)

type Host struct {
	e      *Engine
	files  map[string][]byte
	online atomic.Bool
	reload chan struct{}
	mu     sync.Mutex
	outbox []map[string]any
}

func NewHost(e *Engine) *Host {
	return &Host{e: e, files: MarketFiles(), reload: make(chan struct{}, 1)}
}
func (h *Host) Reload() {
	select {
	case h.reload <- struct{}{}:
	default:
	}
}
func (h *Host) Online() bool { return h.online.Load() }
func (h *Host) enqueue(t Task) {
	reason, mode := "status_changed", "info"
	if t.Status == "waiting_approval" {
		reason, mode = "needs_you", "action"
	} else if t.Status == "completed" {
		reason = "completed"
	} else if t.Status == "running" || t.Status == "submitted" {
		return
	}
	key := sessionKey(t.Provider, t.ID)
	h.mu.Lock()
	defer h.mu.Unlock()
	for _, x := range h.outbox {
		if x["session"] == key && x["reason"] == reason {
			return
		}
	}
	if len(h.outbox) >= 64 {
		h.outbox = h.outbox[1:]
	}
	h.outbox = append(h.outbox, map[string]any{"type": "hint", "app_id": t.Provider + ".status", "reason": reason, "mode": mode, "session": key, "title": clip(statusLabel(t.Status)+" · "+t.Title, 80)})
}
func (h *Host) Run(ctx context.Context) {
	backoff := time.Second
	for ctx.Err() == nil {
		s := h.e.Config.Snapshot()
		if s.Config.HostWS != "" && s.Secrets["host_token"] != "" {
			if h.connection(ctx, s) {
				backoff = time.Second
			}
		}
		timer := time.NewTimer(backoff)
	wait:
		for {
			select {
			case <-ctx.Done():
				timer.Stop()
				return
			case <-h.reload:
				timer.Stop()
				break wait
			case t := <-h.e.Notice:
				h.enqueue(t)
			case <-timer.C:
				break wait
			}
		}
		if backoff < 30*time.Second {
			backoff *= 2
			if backoff > 30*time.Second {
				backoff = 30 * time.Second
			}
		}
	}
}
func (h *Host) connection(parent context.Context, s Settings) bool {
	ctx, cancel := context.WithCancel(parent)
	defer cancel()
	dialer := websocket.Dialer{HandshakeTimeout: 8 * time.Second, Proxy: http.ProxyFromEnvironment}
	c, resp, err := dialer.DialContext(ctx, s.Config.HostWS, http.Header{"X-Bridge-Token": []string{s.Secrets["host_token"]}})
	if err != nil {
		if resp != nil {
			resp.Body.Close()
		}
		return false
	}
	defer c.Close()
	stop := context.AfterFunc(ctx, func() { c.Close() })
	defer stop()
	c.SetReadLimit(2 << 20)
	c.SetReadDeadline(time.Now().Add(90 * time.Second))
	c.SetPongHandler(func(string) error { return c.SetReadDeadline(time.Now().Add(90 * time.Second)) })
	var writeMu sync.Mutex
	write := func(b []byte) error {
		writeMu.Lock()
		defer writeMu.Unlock()
		c.SetWriteDeadline(time.Now().Add(10 * time.Second))
		return c.WriteMessage(websocket.TextMessage, b)
	}
	send := func(v any) error {
		b, err := json.Marshal(v)
		if err != nil {
			return err
		}
		return write(b)
	}
	probe, done := context.WithTimeout(ctx, 5*time.Second)
	states := h.e.Health(probe)
	done()
	if send(map[string]any{"type": "register", "bridge_id": s.Config.BridgeID, "name": s.Config.Name, "platform": runtime.GOOS, "version": Version, "panel_device_id": s.Config.PanelID, "default_provider": s.Config.DefaultProvider, "providers": states}) != nil {
		return false
	}
	frames := make(chan []byte, 16)
	readDone := make(chan struct{})
	go func() {
		defer close(readDone)
		for {
			_, b, err := c.ReadMessage()
			if err != nil {
				return
			}
			c.SetReadDeadline(time.Now().Add(90 * time.Second))
			select {
			case frames <- b:
			case <-ctx.Done():
				return
			}
		}
	}()
	ticker := time.NewTicker(25 * time.Second)
	defer ticker.Stop()
	defer h.online.Store(false)
	workers := make(chan struct{}, 4)
	var wg sync.WaitGroup
	defer func() { cancel(); wg.Wait() }()
	flush := func() error {
		h.mu.Lock()
		defer h.mu.Unlock()
		for len(h.outbox) > 0 {
			if err := send(h.outbox[0]); err != nil {
				return err
			}
			h.outbox = h.outbox[1:]
		}
		return nil
	}
	for {
		select {
		case <-ctx.Done():
			return h.online.Load()
		case <-h.reload:
			return h.online.Load()
		case <-readDone:
			return h.online.Load()
		case t := <-h.e.Notice:
			h.enqueue(t)
			if h.online.Load() {
				if flush() != nil {
					return true
				}
			}
		case <-ticker.C:
			if send(map[string]any{"type": "ping"}) != nil {
				return h.online.Load()
			}
		case raw := <-frames:
			// Host write pump may batch JSON frames with newline separators.
			for _, line := range splitFrames(raw) {
				var f struct {
					Type      string          `json:"type"`
					RequestID string          `json:"request_id"`
					AppID     string          `json:"app_id"`
					Params    json.RawMessage `json:"params"`
					Path      string          `json:"path"`
					Timeout   int             `json:"timeout_ms"`
				}
				if json.Unmarshal(line, &f) != nil {
					continue
				}
				if f.Type == "registered" {
					h.online.Store(true)
					if flush() != nil {
						return true
					}
					continue
				}
				if f.Type != "invoke" && f.Type != "fetch" {
					continue
				}
				if !h.online.Load() {
					continue
				}
				select {
				case workers <- struct{}{}:
				default:
					_ = send(map[string]any{"type": "result", "request_id": f.RequestID, "status_code": 429, "body": errorEnvelope(f.AppID, fmt.Errorf("bridge busy; request not executed"))})
					continue
				}
				wg.Add(1)
				go func() {
					defer wg.Done()
					defer func() { <-workers }()
					if f.Type == "fetch" {
						body, ok := marketLookup(h.files, f.Path)
						if f.Path == MarketPrefix+"marketplace.json" || f.Path == MarketPrefix || f.Path == strings.TrimSuffix(MarketPrefix, "/") {
							probe, done := context.WithTimeout(ctx, 6*time.Second)
							body = h.e.MarketIndex(probe)
							done()
						}
						if !ok {
							_ = send(map[string]any{"type": "result", "request_id": f.RequestID, "status_code": 404, "body": map[string]string{"error": "not_found"}})
							return
						}
						id, _ := json.Marshal(f.RequestID)
						head := []byte(`{"type":"result","status_code":200,"request_id":` + string(id) + `,"body":`)
						message := append(head, body...)
						message = append(message, '}')
						_ = write(message)
						return
					}
					r := Request{RequestID: f.RequestID}
					params := f.Params
					if len(params) > 0 && params[0] == '"' {
						var text string
						if json.Unmarshal(params, &text) == nil {
							params = []byte(text)
						}
					}
					var result map[string]any
					if len(params) > 0 && json.Unmarshal(params, &r) != nil {
						result = errorEnvelope(f.AppID, fmt.Errorf("invalid invoke parameters"))
					} else {
						if r.RequestID == "" {
							r.RequestID = f.RequestID
						}
						timeout := 22 * time.Second
						if f.Timeout > 0 && f.Timeout < 22000 {
							timeout = time.Duration(f.Timeout) * time.Millisecond
						}
						callCtx, done := context.WithTimeout(ctx, timeout)
						result = h.e.Invoke(callCtx, f.AppID, r)
						done()
					}
					_ = send(map[string]any{"type": "result", "request_id": f.RequestID, "status_code": 200, "body": result})
				}()
			}
		}
	}
}
func splitFrames(b []byte) [][]byte {
	var out [][]byte
	start := 0
	for i, c := range b {
		if c == '\n' {
			if i > start {
				out = append(out, b[start:i])
			}
			start = i + 1
		}
	}
	if start < len(b) {
		out = append(out, b[start:])
	}
	return out
}
