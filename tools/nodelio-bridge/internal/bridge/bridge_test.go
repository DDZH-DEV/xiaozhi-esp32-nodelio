package bridge

import (
	"bufio"
	"bytes"
	"context"
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"fmt"
	"io"
	"net"
	"net/http"
	"net/http/httptest"
	"os"
	"path/filepath"
	"strings"
	"sync"
	"sync/atomic"
	"testing"
	"time"

	"github.com/gorilla/websocket"
	"github.com/klauspost/compress/zstd"
)

func fixture(t *testing.T) (*Engine, context.CancelFunc) {
	t.Helper()
	dir := t.TempDir()
	cfg, err := OpenConfig(dir)
	if err != nil {
		t.Fatal(err)
	}
	state, err := OpenState(dir)
	if err != nil {
		t.Fatal(err)
	}
	ctx, cancel := context.WithCancel(context.Background())
	e := NewEngine(ctx, cfg, state)
	e.Ledger, err = OpenLedger(dir)
	if err != nil {
		cancel()
		t.Fatal(err)
	}
	t.Cleanup(func() { cancel(); e.Wait() })
	return e, cancel
}
func configure(t *testing.T, e *Engine, id, base string) {
	t.Helper()
	s := e.Config.Snapshot()
	p := s.Config.Providers[id]
	p.Enabled = true
	if base != "" {
		p.BaseURL = base
	}
	if id == "openai" {
		p.Model = "test-model"
	}
	s.Config.Providers[id] = p
	if err := e.Config.Update(ConfigUpdate{Config: s.Config, Secrets: map[string]string{"openai_key": "test-key", "workbuddy_token": "test-token"}}); err != nil {
		t.Fatal(err)
	}
}
func TestConfigPersistenceAndRejection(t *testing.T) {
	e, _ := fixture(t)
	before := e.Config.Snapshot()
	s := before.Config
	s.Providers["openai"] = ProviderConfig{Enabled: true, BaseURL: "https://api.openai.com/v1", Model: "test"}
	if err := e.Config.Update(ConfigUpdate{Config: s, Secrets: map[string]string{"openai_key": "secret-value"}}); err != nil {
		t.Fatal(err)
	}
	reopen, err := OpenConfig(e.Config.dir)
	if err != nil {
		t.Fatal(err)
	}
	if reopen.Snapshot().Config.BridgeID != before.Config.BridgeID {
		t.Fatal("identity changed")
	}
	s.Listen = "0.0.0.0:8791"
	if e.Config.Update(ConfigUpdate{Config: s}) == nil {
		t.Fatal("accepted exposed management listener")
	}
	if e.Config.Snapshot().Secrets["openai_key"] != "secret-value" {
		t.Fatal("bad update damaged credentials")
	}
	s = e.Config.Snapshot().Config
	s.Providers["openai"] = ProviderConfig{BaseURL: "https://user:pass@example.com"}
	if ValidateConfig(s) == nil {
		t.Fatal("URL credentials accepted")
	}
	if _, err := os.Stat(filepath.Join(e.Config.dir, "settings.json")); err != nil {
		t.Fatal(err)
	}
}
func TestManagementAuthorizationAndCSRF(t *testing.T) {
	e, _ := fixture(t)
	s := &Server{Engine: e, Host: NewHost(e), Dir: e.Config.dir, Listen: "127.0.0.1:8791"}
	cases := []struct {
		host, token, origin string
		want                int
	}{
		{"127.0.0.1:8791", "", "", 401},
		{"evil.example:8791", e.Config.Snapshot().Secrets["admin_token"], "", 403},
		{"127.0.0.1:8791", e.Config.Snapshot().Secrets["admin_token"], "https://evil.example", 403},
		{"127.0.0.1:8791", e.Config.Snapshot().Secrets["admin_token"], "http://127.0.0.1:8791", 200},
	}
	for _, c := range cases {
		r := httptest.NewRequest("GET", "http://"+c.host+"/api/config", nil)
		r.Header.Set("Authorization", "Bearer "+c.token)
		r.Header.Set("Origin", c.origin)
		w := httptest.NewRecorder()
		s.Handler().ServeHTTP(w, r)
		if w.Code != c.want {
			t.Fatalf("%+v: %d", c, w.Code)
		}
		if w.Code == 200 && strings.Contains(w.Body.String(), c.token) {
			t.Fatal("admin token leaked")
		}
	}
}
func TestProviderCannotBeOverriddenBySession(t *testing.T) {
	e, _ := fixture(t)
	configure(t, e, "openai", "")
	_, err := e.Dispatch(context.Background(), "task", Request{Provider: "openai", Session: sessionKey("dsh", "123"), Instruction: "hello"})
	if err == nil {
		t.Fatal("cross-provider dispatch accepted")
	}
	_, err = e.Dispatch(context.Background(), "task", Request{Provider: "dsh", Instruction: "hello"})
	if err == nil {
		t.Fatal("disabled provider executed")
	}
}
func TestOpenAIAsyncAndContinue(t *testing.T) {
	var requests atomic.Int32
	release := make(chan struct{})
	var mu sync.Mutex
	var previous string
	upstream := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		if r.URL.Path != "/responses" {
			http.NotFound(w, r)
			return
		}
		if r.Header.Get("Authorization") != "Bearer test-key" {
			t.Error("missing auth")
		}
		var body map[string]any
		_ = json.NewDecoder(r.Body).Decode(&body)
		mu.Lock()
		previous = str(body["previous_response_id"])
		mu.Unlock()
		n := requests.Add(1)
		if n == 1 {
			<-release
		}
		fmt.Fprintf(w, `{"id":"resp_%d","status":"completed","output":[{"type":"message","content":[{"type":"output_text","text":"answer"}]}]}`, n)
	}))
	defer upstream.Close()
	e, _ := fixture(t)
	configure(t, e, "openai", upstream.URL)
	r := Request{Provider: "openai", Instruction: "hello", RequestID: "once"}
	tasks, err := e.Dispatch(context.Background(), "task", r)
	if err != nil {
		t.Fatal(err)
	}
	if tasks[0].Status != "running" {
		t.Fatal("should return before completion")
	}
	_, err = e.Dispatch(context.Background(), "task", r)
	if err != nil {
		t.Fatal(err)
	}
	close(release)
	e.Wait()
	if requests.Load() != 1 {
		t.Fatal("duplicate mutation")
	}
	key := tasks[0].ID
	tasks, err = e.Dispatch(context.Background(), "status", Request{Session: key})
	if err != nil || tasks[0].Body != "answer" {
		t.Fatalf("%+v %v", tasks, err)
	}
	_, err = e.Dispatch(context.Background(), "task", Request{Session: key, Instruction: "continue", RequestID: "second"})
	if err != nil {
		t.Fatal(err)
	}
	e.Wait()
	mu.Lock()
	defer mu.Unlock()
	if previous != "resp_1" {
		t.Fatalf("lost continuation: %s", previous)
	}
}
func TestRestartDoesNotReplayUnknownJob(t *testing.T) {
	e, _ := fixture(t)
	task := Task{ID: "one", Provider: "openai", Status: "running", RequestID: "req"}
	if err := e.State.Put(task); err != nil {
		t.Fatal(err)
	}
	s, err := OpenState(e.Config.dir)
	if err != nil {
		t.Fatal(err)
	}
	got, _ := s.Get("one")
	if got.Status != "unknown" {
		t.Fatalf("%+v", got)
	}
}
func TestSSEMultilineAndEOF(t *testing.T) {
	input := ": keepalive\r\nevent: message\r\ndata: {\"id\":1,\r\ndata: \"result\":{}}\r\n\r\ndata: {\"id\":2,\"result\":{}}"
	ch := make(chan acpFrame, 2)
	err := readSSE(context.Background(), strings.NewReader(input), ch)
	if err != io.EOF {
		t.Fatal(err)
	}
	if string((<-ch).ID) != "1" || string((<-ch).ID) != "2" {
		t.Fatal("bad SSE parsing")
	}
}
func TestWorkBuddyCloudRoundTrip(t *testing.T) {
	frames := make(chan []byte, 16)
	var upstream *httptest.Server
	var prompts atomic.Int32
	upstream = httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		switch r.URL.Path {
		case "/tasks":
			fmt.Fprintf(w, `{"task_id":"remote-1","link":%q,"token":"ticket"}`, upstream.URL+"/acp")
		case "/acp":
			if r.Method == "GET" {
				w.Header().Set("Content-Type", "text/event-stream")
				w.Header().Set("Acp-Connection-Id", "conn-1")
				w.WriteHeader(200)
				w.(http.Flusher).Flush()
				for {
					select {
					case b := <-frames:
						fmt.Fprintf(w, "data: %s\n\n", b)
						w.(http.Flusher).Flush()
					case <-r.Context().Done():
						return
					}
				}
			}
			if r.Header.Get("Acp-Connection-Id") != "conn-1" {
				t.Error("missing connection ID")
			}
			var msg map[string]any
			_ = json.NewDecoder(r.Body).Decode(&msg)
			if str(msg["method"]) == "session/prompt" {
				prompts.Add(1)
				frames <- []byte(`{"method":"session/update","params":{"update":{"sessionUpdate":"agent_message_chunk","content":{"text":"cloud answer"}}}}`)
				frames <- []byte(`{"id":3,"result":{"stopReason":"end_turn"}}`)
			} else {
				frames <- marshalRaw(map[string]any{"id": msg["id"], "result": map[string]any{}})
			}
			w.WriteHeader(202)
		default:
			http.NotFound(w, r)
		}
	}))
	defer upstream.Close()
	e, _ := fixture(t)
	configure(t, e, "workbuddy", upstream.URL)
	s := e.Config.Snapshot()
	cfg := s.Config.Providers["workbuddy"]
	cfg.Mode = "cloud"
	s.Config.Providers["workbuddy"] = cfg
	if err := e.Config.Update(ConfigUpdate{Config: s.Config}); err != nil {
		t.Fatal(err)
	}
	tasks, err := e.Dispatch(context.Background(), "task", Request{Provider: "workbuddy", Instruction: "hello"})
	if err != nil {
		t.Fatal(err)
	}
	e.Wait()
	result, err := e.Dispatch(context.Background(), "status", Request{Session: tasks[0].ID})
	if err != nil || len(result) != 1 || result[0].Body != "cloud answer" || result[0].Status != "completed" {
		t.Fatalf("%+v %v", result, err)
	}
	if prompts.Load() != 1 {
		t.Fatal("prompt sent more than once")
	}
}
func TestDSHRPCExactSessionAndStaleApproval(t *testing.T) {
	listener, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		t.Fatal(err)
	}
	defer listener.Close()
	var mutations atomic.Int32
	go func() {
		for {
			conn, err := listener.Accept()
			if err != nil {
				return
			}
			go func() {
				defer conn.Close()
				scan := bufio.NewScanner(conn)
				for scan.Scan() {
					var req map[string]any
					_ = json.Unmarshal(scan.Bytes(), &req)
					var result any = map[string]any{}
					switch str(req["method"]) {
					case "session.list":
						result = map[string]any{"sessions": []any{map[string]any{"sessionId": "internal", "externalSessionId": "exact-session", "title": "test"}}}
					case "session.getNotices":
						result = map[string]any{"notices": []any{map[string]any{"id": "current", "actions": []any{map[string]any{"actionId": "allow"}}}}}
					case "session.startTurn", "session.respondInteraction":
						mutations.Add(1)
					}
					b, _ := json.Marshal(map[string]any{"id": req["id"], "result": result})
					fmt.Fprintf(conn, "%s\n", b)
				}
			}()
		}
	}()
	e, _ := fixture(t)
	endpoint := filepath.Join(t.TempDir(), "endpoint.json")
	if err = writeJSON(endpoint, map[string]any{"host": "127.0.0.1", "port": listener.Addr().(*net.TCPAddr).Port, "token": "fake"}); err != nil {
		t.Fatal(err)
	}
	s := e.Config.Snapshot()
	cfg := s.Config.Providers["dsh"]
	cfg.Enabled = true
	cfg.EndpointFile = endpoint
	s.Config.Providers["dsh"] = cfg
	if err = e.Config.Update(ConfigUpdate{Config: s.Config}); err != nil {
		t.Fatal(err)
	}
	_, err = e.Dispatch(context.Background(), "task", Request{Session: sessionKey("dsh", "exact"), Instruction: "wrong"})
	if err == nil {
		t.Fatal("partial ID matched")
	}
	_, err = e.Dispatch(context.Background(), "answer", Request{Session: sessionKey("dsh", "exact-session"), NoticeID: "stale", Action: "allow"})
	if err == nil {
		t.Fatal("stale approval accepted")
	}
	_, err = e.Dispatch(context.Background(), "task", Request{Session: sessionKey("dsh", "exact-session"), Instruction: "ok"})
	if err != nil {
		t.Fatal(err)
	}
	if mutations.Load() != 1 {
		t.Fatal("unexpected mutation")
	}
}
func TestDSHConcatenatedZstdAndTornTail(t *testing.T) {
	home := t.TempDir()
	cwd := "C:/project"
	dir := filepath.Join(home, "sessions", "--C-project--", "sid")
	if err := os.MkdirAll(dir, 0700); err != nil {
		t.Fatal(err)
	}
	encoder, err := zstd.NewWriter(nil)
	if err != nil {
		t.Fatal(err)
	}
	defer encoder.Close()
	b := encoder.EncodeAll([]byte("{\"type\":\"user/message\",\"data\":{\"content\":\"question\"}}\n"), nil)
	b = append(b, encoder.EncodeAll([]byte("{\"type\":\"assistant/message\",\"data\":{\"content\":[{\"type\":\"text\",\"text\":\"answer\"}]}}\n"), nil)...)
	b = append(b, 0x28, 0xb5, 0x2f, 0xfd, 0)
	if err = os.WriteFile(filepath.Join(dir, "session.v4.jsonl.zstd"), b, 0600); err != nil {
		t.Fatal(err)
	}
	logs := &dshLogs{entries: map[string]logEntry{}}
	got := logs.read(home, cwd, "sid")
	if got.User != "question" || got.Body != "answer" {
		t.Fatalf("%+v", got)
	}
}
func TestHostFetchPreservesPackageHash(t *testing.T) {
	e, cancel := fixture(t)
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
		_ = c.WriteJSON(map[string]any{"type": "registered"})
		path := MarketPrefix + "packages/openai.status/" + PackageVersion + ".json"
		_ = c.WriteJSON(map[string]any{"type": "fetch", "request_id": "fetch-1", "path": path})
		_, raw, err := c.ReadMessage()
		if err != nil {
			done <- err
			return
		}
		var frame struct {
			Body json.RawMessage `json:"body"`
		}
		if err = json.Unmarshal(raw, &frame); err != nil {
			done <- err
			return
		}
		if !bytes.Equal(frame.Body, MarketFiles()[path]) {
			done <- fmt.Errorf("package bytes changed")
			return
		}
		done <- nil
		<-r.Context().Done()
	}))
	defer upstream.Close()
	s := e.Config.Snapshot()
	s.Config.HostWS = "ws" + strings.TrimPrefix(upstream.URL, "http")
	if err := e.Config.Update(ConfigUpdate{Config: s.Config, Secrets: map[string]string{"host_token": "fake"}}); err != nil {
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
}
func TestMarketHashesAndTraversal(t *testing.T) {
	files := MarketFiles()
	var index struct {
		Apps []struct {
			URL string `json:"package_url"`
			SHA string `json:"package_sha256"`
		} `json:"apps"`
	}
	if err := json.Unmarshal(files[MarketPrefix+"marketplace.json"], &index); err != nil {
		t.Fatal(err)
	}
	for _, app := range index.Apps {
		b, ok := marketLookup(files, MarketPrefix+strings.TrimPrefix(app.URL, "nodelio-dsh://"))
		if !ok {
			t.Fatal("missing package")
		}
		hash := sha256.Sum256(b)
		if hex.EncodeToString(hash[:]) != app.SHA {
			t.Fatal("hash mismatch")
		}
	}
	if _, ok := marketLookup(files, MarketPrefix+"../settings.json"); ok {
		t.Fatal("traversal")
	}
}
func TestStartupDocumentsAndInstanceLock(t *testing.T) {
	dir := t.TempDir()
	unlock, err := LockInstance(dir)
	if err != nil {
		t.Fatal(err)
	}
	if second, e := LockInstance(dir); e == nil {
		second()
		t.Fatal("second instance acquired lock")
	}
	unlock()
	for _, goos := range []string{"windows", "darwin", "linux"} {
		doc, err := StartupDocument(goos, `C:\Apps & Tools\bridge.exe`, dir, "user")
		if err != nil {
			t.Fatal(err)
		}
		if !strings.Contains(doc, "serve") {
			t.Fatal("missing serve command")
		}
		if goos != "linux" && !strings.Contains(doc, "&amp;") {
			t.Fatal("unescaped XML")
		}
	}
	doc, _ := StartupDocument("linux", "/tmp/100% tool/$binary", dir, "1000")
	if !strings.Contains(doc, "100%%") || !strings.Contains(doc, "$$binary") {
		t.Fatal("systemd expansion not escaped")
	}
}
