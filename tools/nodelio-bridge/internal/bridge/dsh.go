package bridge

import (
	"bufio"
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"net"
	"os"
	"path/filepath"
	"runtime"
	"strings"
	"time"
)

type DSH struct {
	config *ConfigStore
	gate   chan struct{}
	logs   *dshLogs
}
type dshRPC struct {
	conn   net.Conn
	reader *bufio.Scanner
	next   int
	ctx    context.Context
	mesh   *dshMesh
	v2     bool
}
type dshSession struct {
	ID       string `json:"sessionId"`
	External string `json:"externalSessionId"`
	Title    string `json:"title"`
	CWD      string `json:"cwd"`
}

func NewDSH(c *ConfigStore) *DSH {
	return &DSH{config: c, gate: make(chan struct{}, 1), logs: &dshLogs{entries: map[string]logEntry{}}}
}
func (p *DSH) with(ctx context.Context, fn func(*dshRPC) error) error {
	select {
	case p.gate <- struct{}{}:
	case <-ctx.Done():
		return ctx.Err()
	}
	defer func() {
		select {
		case <-time.After(80 * time.Millisecond):
		case <-ctx.Done():
		}
		<-p.gate
	}()
	if rpc, ok, err := p.dialLegacy(ctx); ok {
		if err != nil {
			return err
		}
		defer rpc.conn.Close()
		return fn(rpc)
	}
	mesh := getDSHMesh()
	if err := mesh.ensure(ctx, p.config.Dir()); err != nil {
		return err
	}
	return fn(&dshRPC{ctx: ctx, mesh: mesh, v2: true})
}

func (p *DSH) dialLegacy(ctx context.Context) (*dshRPC, bool, error) {
	s := p.config.Snapshot()
	ep := s.Config.Providers["dsh"].EndpointFile
	home, err := os.UserHomeDir()
	if err != nil {
		return nil, false, err
	}
	paths := []string{}
	if ep != "" {
		paths = append(paths, ep)
	} else {
		paths = []string{
			filepath.Join(home, ".agents-anywhere", "dsh-bridge", "endpoint.json"),
			filepath.Join(home, ".agents-anywhere", "dsh-bridge-next", "endpoint.json"),
			filepath.Join(home, ".dsh", "agents-anywhere", "bridge", "endpoint.json"),
		}
	}
	var endpoint struct {
		Host  string `json:"host"`
		Port  int    `json:"port"`
		Token string `json:"token"`
	}
	found := false
	for _, name := range paths {
		b, e := os.ReadFile(name)
		if e == nil && json.Unmarshal(b, &endpoint) == nil && endpoint.Port > 0 && endpoint.Port < 65536 && endpoint.Token != "" {
			found = true
			break
		}
	}
	if !found {
		return nil, false, nil
	}
	if endpoint.Host == "" {
		endpoint.Host = "127.0.0.1"
	}
	ip := net.ParseIP(endpoint.Host)
	if ip == nil || !ip.IsLoopback() {
		return nil, true, errors.New("DSH endpoint must be loopback")
	}
	conn, err := (&net.Dialer{Timeout: 3 * time.Second}).DialContext(ctx, "tcp", net.JoinHostPort(endpoint.Host, fmt.Sprint(endpoint.Port)))
	if err != nil {
		return nil, true, errors.New("DSH desktop is offline")
	}
	stop := context.AfterFunc(ctx, func() { conn.Close() })
	scanner := bufio.NewScanner(conn)
	scanner.Buffer(make([]byte, 4096), 4<<20)
	rpc := &dshRPC{conn: conn, reader: scanner, ctx: ctx, v2: false}
	var init any
	if err = rpc.call("initialize", map[string]any{"authToken": endpoint.Token, "protocolVersion": "1.0", "runtime": "dsh", "sessionNamespace": "nodelio-bridge"}, &init); err != nil {
		stop()
		conn.Close()
		return nil, true, err
	}
	return rpc, true, nil
}

func (c *dshRPC) call(method string, params any, out any) error {
	if c.v2 {
		return c.mesh.call(c.ctx, dshMethodV2(method), params, out)
	}
	c.next++
	deadline := time.Now().Add(20 * time.Second)
	if method == "initialize" {
		deadline = time.Now().Add(5 * time.Second)
	}
	if d, ok := c.ctx.Deadline(); ok && d.Before(deadline) {
		deadline = d
	}
	c.conn.SetDeadline(deadline)
	b, err := json.Marshal(map[string]any{"jsonrpc": "2.0", "id": c.next, "method": method, "params": params})
	if err != nil {
		return err
	}
	if _, err = c.conn.Write(append(b, '\n')); err != nil {
		return errors.New("DSH write failed; outcome may be unknown")
	}
	for c.reader.Scan() {
		var msg struct {
			ID     int             `json:"id"`
			Result json.RawMessage `json:"result"`
			Error  *struct {
				Message string `json:"message"`
			} `json:"error"`
		}
		if json.Unmarshal(c.reader.Bytes(), &msg) != nil {
			continue
		}
		if msg.ID != c.next {
			continue
		}
		if msg.Error != nil {
			return fmt.Errorf("DSH %s: %s", method, clip(msg.Error.Message, 200))
		}
		var probe map[string]any
		_ = json.Unmarshal(msg.Result, &probe)
		if probe["success"] == false || probe["ok"] == false || obj(probe["result"])["success"] == false {
			return fmt.Errorf("DSH %s rejected the request", method)
		}
		if out != nil {
			return json.Unmarshal(msg.Result, out)
		}
		return nil
	}
	return fmt.Errorf("DSH %s connection closed or timed out; do not automatically resend", method)
}
func sessions(c *dshRPC) ([]dshSession, error) {
	var d struct {
		Sessions []dshSession `json:"sessions"`
	}
	err := c.call("session.list", map[string]any{}, &d)
	return d.Sessions, err
}
func findSession(list []dshSession, id string) (dshSession, error) {
	for _, s := range list {
		if s.External == id || s.ID == id {
			return s, nil
		}
	}
	return dshSession{}, errors.New("DSH session not found")
}
func (p *DSH) Health(ctx context.Context) Health {
	h := Health{State: "ready", Message: "DSH 已连接", Capabilities: []string{"list", "create", "continue", "approval"}}
	if err := p.with(ctx, func(c *dshRPC) error { return nil }); err != nil {
		h.State = "offline"
		h.Message = dshHealthMessage(err)
	}
	return h
}

func dshHealthMessage(err error) string {
	if err == nil {
		return ""
	}
	msg := err.Error()
	if errors.Is(err, context.DeadlineExceeded) || strings.Contains(msg, "context deadline exceeded") || strings.Contains(msg, "DSH bridge not registered") || strings.Contains(msg, "No instances registered for aa.dsh.v2.Bridge") {
		return "DSH 未注册 Bridge。打开 DSH NEXT → 插件 → 远程控制，打开「启用远程控制」，按提示重启后台服务后再检查"
	}
	return msg
}
func (p *DSH) read(c *dshRPC, s dshSession) (Task, error) {
	t := Task{ID: s.External, Provider: "dsh", Title: s.Title, Workspace: s.CWD, Updated: time.Now().UnixMilli(), Status: "unknown"}
	var state map[string]any
	if err := c.call("session.getState", map[string]any{"externalSessionId": s.External}, &state); err != nil {
		return t, err
	}
	t.Status = str(state["status"])
	home := p.config.Snapshot().Config.Providers["dsh"].DSHHome
	if home == "" {
		u, _ := os.UserHomeDir()
		home = filepath.Join(u, ".dsh")
	}
	log := p.logs.read(home, s.CWD, s.External)
	if log.Title != "" {
		t.Title = log.Title
	}
	if log.User != "" {
		t.Title = clip(log.User, 96)
	}
	t.Body = log.Body
	var notices struct {
		Notices []map[string]any `json:"notices"`
	}
	if err := c.call("session.getNotices", map[string]any{"externalSessionId": s.External}, &notices); err != nil {
		return t, err
	}
	for _, n := range notices.Notices {
		id := str(n["id"])
		if id == "" {
			id = str(n["noticeId"])
		}
		if id == "" {
			continue
		}
		nt := Notice{ID: id, Title: str(n["title"]), Options: []Option{}}
		for _, a := range arr(n["actions"]) {
			action := obj(a)
			aid := str(action["actionId"])
			if aid == "" {
				aid = str(action["id"])
			}
			// Structured questions are displayed on the desktop until a schema-aware UI is available.
			if aid == "submit" || str(n["interactionType"]) == "input_request" {
				nt.Title = "请在 DSH 桌面回答此问题"
				continue
			}
			if aid != "" {
				title := str(action["title"])
				if title == "" {
					title = aid
				}
				nt.Options = append(nt.Options, Option{ID: aid, Title: title})
			}
		}
		t.Notices = append(t.Notices, nt)
	}
	return t, nil
}
func (p *DSH) List(ctx context.Context, r Request) ([]Task, error) {
	out := []Task{}
	err := p.with(ctx, func(c *dshRPC) error {
		list, err := sessions(c)
		if err != nil {
			return err
		}
		for _, s := range list {
			if r.Session != "" && r.Session != s.External && r.Session != s.ID {
				continue
			}
			if r.Session == "" && workspaceSelected(r.Workspace) && !sameWorkspace(s.CWD, r.Workspace) {
				continue
			}
			t, err := p.read(c, s)
			if err != nil {
				return err
			}
			out = append(out, t)
			if len(out) >= 8 {
				break
			}
		}
		return nil
	})
	return out, err
}
func (p *DSH) Send(ctx context.Context, r Request) (Task, error) {
	var result Task
	err := p.with(ctx, func(c *dshRPC) error {
		list, err := sessions(c)
		if err != nil {
			return err
		}
		if r.Session != "" {
			s, err := findSession(list, r.Session)
			if err != nil {
				return err
			}
			err = c.call("session.startTurn", map[string]any{"sessionId": s.ID, "externalSessionId": s.External, "content": r.Instruction, "clientMessageId": NewID()}, nil)
			if err != nil {
				return err
			}
			result = Task{ID: s.External, Provider: "dsh", Title: clip(r.Instruction, 96), Workspace: s.CWD, Status: "running"}
			return nil
		}
		cfg := p.config.Snapshot().Config.Providers["dsh"]
		cwd := r.Workspace
		if cwd == "" {
			cwd = cfg.Workspace
		}
		if !filepath.IsAbs(cwd) {
			return errors.New("choose an absolute DSH workspace path")
		}
		info, err := os.Stat(cwd)
		if err != nil || !info.IsDir() {
			return errors.New("DSH workspace does not exist")
		}
		selection, err := dshSelections(c, cfg)
		if err != nil {
			return err
		}
		var created map[string]any
		err = c.call("session.createAndStart", map[string]any{"sessionId": NewID(), "content": r.Instruction, "cwd": cwd, "clientMessageId": NewID(), "selections": selection, "agentPreset": cfg.AgentPreset}, &created)
		if err != nil {
			return err
		}
		id := str(created["externalSessionId"])
		if id == "" {
			id = str(created["sessionId"])
		}
		if id == "" {
			id = str(obj(created["result"])["externalSessionId"])
		}
		if id == "" {
			id = str(obj(created["result"])["sessionId"])
		}
		after, err := sessions(c)
		if err != nil {
			return err
		}
		var candidates []dshSession
		for _, s := range after {
			if id != "" && (s.External == id || s.ID == id) {
				candidates = []dshSession{s}
				break
			}
			old := false
			for _, before := range list {
				if before.External == s.External {
					old = true
					break
				}
			}
			if !old && filepath.Clean(s.CWD) == filepath.Clean(cwd) {
				candidates = append(candidates, s)
			}
		}
		if len(candidates) != 1 {
			return errors.New("DSH create outcome ambiguous; inspect desktop before retrying")
		}
		s := candidates[0]
		result = Task{ID: s.External, Provider: "dsh", Title: clip(r.Instruction, 96), Workspace: s.CWD, Status: "running"}
		return nil
	})
	return result, err
}
func dshSelections(c *dshRPC, cfg ProviderConfig) (map[string]string, error) {
	var models struct {
		Models []map[string]any `json:"models"`
	}
	var permissions struct {
		Permissions []map[string]any `json:"permissions"`
	}
	if err := c.call("catalog.listModels", map[string]any{"limit": 1000}, &models); err != nil {
		return nil, err
	}
	if err := c.call("catalog.listPermissions", map[string]any{"limit": 100}, &permissions); err != nil {
		return nil, err
	}
	model, permission := "", ""
	for _, m := range models.Models {
		if m["enabled"] == false {
			continue
		}
		meta := obj(m["metadata"])
		if cfg.Model == "" || cfg.Model == str(m["id"]) || cfg.Model == str(m["selectionId"]) || cfg.Model == str(m["title"]) || cfg.Model == str(meta["model"]) || cfg.Model == str(meta["provider"])+"/"+str(meta["model"]) {
			model = str(m["selectionId"])
			if model == "" {
				model = str(m["id"])
			}
			break
		}
	}
	for _, m := range permissions.Permissions {
		if m["enabled"] == false {
			continue
		}
		if cfg.Permission != "" && (cfg.Permission == str(m["id"]) || cfg.Permission == str(m["selectionId"]) || cfg.Permission == str(obj(m["metadata"])["preset"]) || cfg.Permission == str(m["title"])) {
			permission = str(m["selectionId"])
			break
		}
	}
	if model == "" || permission == "" {
		return nil, errors.New("select a valid DSH model and explicit permission preset; permissions are never broadened automatically")
	}
	return map[string]string{"model": model, "permission": permission}, nil
}
func (p *DSH) Answer(ctx context.Context, r Request) error {
	return p.with(ctx, func(c *dshRPC) error {
		list, err := sessions(c)
		if err != nil {
			return err
		}
		s, err := findSession(list, r.Session)
		if err != nil {
			return err
		}
		var doc struct {
			Notices []map[string]any `json:"notices"`
		}
		if err = c.call("session.getNotices", map[string]any{"externalSessionId": s.External}, &doc); err != nil {
			return err
		}
		allowed := false
		for _, n := range doc.Notices {
			if str(n["id"]) != r.NoticeID && str(n["noticeId"]) != r.NoticeID {
				continue
			}
			for _, a := range arr(n["actions"]) {
				m := obj(a)
				if str(m["actionId"]) == r.Action || str(m["id"]) == r.Action {
					allowed = true
				}
			}
		}
		if !allowed {
			return errors.New("approval expired or action does not belong to this notice")
		}
		params := map[string]any{"sessionId": s.ID, "externalSessionId": s.External, "noticeId": r.NoticeID, "actionId": r.Action}
		if r.Action == "submit" {
			if len(r.Answers) == 0 {
				return errors.New("answers required")
			}
			params["inputData"] = map[string]any{"answers": r.Answers}
		}
		if err = c.call("session.respondInteraction", params, nil); err != nil {
			return err
		}
		if r.Action == "deny" || r.Action == "reject" || r.Action == "cancel" {
			return c.call("session.interrupt", map[string]any{"sessionId": s.ID, "externalSessionId": s.External, "reason": "nodelio-" + r.Action}, nil)
		}
		return nil
	})
}

func (p *DSH) Choices(ctx context.Context) map[string]any {
	cfg := p.config.Snapshot().Config.Providers["dsh"]
	result := map[string]any{"workspaces": []Choice{}, "models": []Choice{}, "permissions": []Choice{}, "agents": defaultAgentChoices()}
	var sessionPaths []string
	err := p.with(ctx, func(c *dshRPC) error {
		models, permissions, agents, paths, err := dshCatalog(c)
		if err != nil {
			return err
		}
		result["models"] = models
		result["permissions"] = permissions
		if len(agents) > 0 {
			result["agents"] = agents
		}
		sessionPaths = paths
		return nil
	})
	if err != nil {
		result["error"] = err.Error()
	}
	result["workspaces"] = suggestWorkspaces(append([]string{cfg.Workspace}, sessionPaths...))
	return result
}
func dshCatalog(c *dshRPC) (models, permissions, agents []Choice, paths []string, err error) {
	var modelDoc struct {
		Models []map[string]any `json:"models"`
	}
	var permDoc struct {
		Permissions []map[string]any `json:"permissions"`
	}
	if err = c.call("catalog.listModels", map[string]any{"limit": 1000}, &modelDoc); err != nil {
		return
	}
	if err = c.call("catalog.listPermissions", map[string]any{"limit": 100}, &permDoc); err != nil {
		return
	}
	for _, m := range modelDoc.Models {
		if m["enabled"] == false {
			continue
		}
		meta := obj(m["metadata"])
		value := strings.Trim(str(meta["provider"])+"/"+str(meta["model"]), "/")
		if value == "" {
			value = str(m["selectionId"])
		}
		if value == "" {
			value = str(m["id"])
		}
		if value == "" {
			continue
		}
		label := str(m["title"])
		if label == "" {
			label = value
		}
		models = append(models, Choice{Value: value, Label: label})
	}
	for _, m := range permDoc.Permissions {
		if m["enabled"] == false {
			continue
		}
		meta := obj(m["metadata"])
		value := str(meta["preset"])
		if value == "" {
			value = str(m["selectionId"])
		}
		if value == "" {
			value = str(m["id"])
		}
		if value == "" {
			value = str(m["title"])
		}
		if value == "" {
			continue
		}
		label := str(m["title"])
		if label == "" || label == value {
			label = value
		} else {
			label = value + " · " + label
		}
		permissions = append(permissions, Choice{Value: value, Label: label})
	}
	var presetDoc struct {
		Presets []map[string]any `json:"presets"`
	}
	if c.call("catalog.listAgentPresets", map[string]any{}, &presetDoc) == nil {
		for _, m := range presetDoc.Presets {
			if m["enabled"] == false {
				continue
			}
			id := str(m["id"])
			if id == "" {
				continue
			}
			name := str(m["name"])
			if name == "" {
				name = id
			}
			agents = append(agents, Choice{Value: id, Label: name})
		}
	}
	if list, e := sessions(c); e == nil {
		for _, s := range list {
			if s.CWD != "" {
				paths = append(paths, s.CWD)
			}
		}
	}
	return
}
func defaultAgentChoices() []Choice {
	return []Choice{{Value: "standard", Label: "standard"}, {Value: "ptc", Label: "ptc"}, {Value: "minimal", Label: "minimal"}, {Value: "cordis", Label: "cordis"}}
}
func workspaceSelected(path string) bool {
	path = strings.TrimSpace(path)
	return path != "" && path != "__all__"
}

func sameWorkspace(a, b string) bool {
	a, b = filepath.Clean(a), filepath.Clean(b)
	if runtime.GOOS == "windows" {
		return strings.EqualFold(a, b)
	}
	return a == b
}

// WorkspaceFilters 是进度卡顶栏。全部会话始终在，点某个目录只看该工作区。
func (p *DSH) WorkspaceFilters(ctx context.Context, selected, preferred string) []any {
	paths := []string{}
	_ = p.with(ctx, func(c *dshRPC) error {
		list, err := sessions(c)
		if err != nil {
			return err
		}
		for _, s := range list {
			if s.CWD != "" {
				paths = append(paths, s.CWD)
			}
		}
		return nil
	})
	cfg := p.config.Snapshot().Config.Providers["dsh"].Workspace
	raw := append([]string{preferred, cfg, selected}, paths...)
	seen := map[string]bool{}
	ordered := []string{}
	for _, pth := range raw {
		if !workspaceSelected(pth) {
			continue
		}
		pth = filepath.Clean(pth)
		key := pth
		if runtime.GOOS == "windows" {
			key = strings.ToLower(pth)
		}
		if seen[key] {
			continue
		}
		seen[key] = true
		ordered = append(ordered, pth)
		if len(ordered) >= 7 {
			break
		}
	}
	if len(ordered) == 0 {
		return nil
	}
	bases := map[string]int{}
	for _, pth := range ordered {
		bases[strings.ToLower(filepath.Base(pth))]++
	}
	allSelected := !workspaceSelected(selected)
	out := []any{map[string]any{"id": "__all__", "title": "全部", "workspace": "__all__", "selected": allSelected}}
	for _, pth := range ordered {
		label := filepath.Base(pth)
		if bases[strings.ToLower(label)] > 1 {
			parent := filepath.Base(filepath.Dir(pth))
			if parent != "" && parent != "." && parent != string(filepath.Separator) {
				label = parent + "/" + label
			}
		}
		out = append(out, map[string]any{
			"id": pth, "title": label, "workspace": pth, "selected": !allSelected && sameWorkspace(pth, selected),
		})
	}
	return out
}

func suggestWorkspaces(extra []string) []Choice {
	seen := map[string]bool{}
	out := []Choice{}
	add := func(p string) {
		if len(out) >= 40 || strings.TrimSpace(p) == "" {
			return
		}
		p = filepath.Clean(p)
		key := p
		if runtime.GOOS == "windows" {
			key = strings.ToLower(p)
		}
		if seen[key] {
			return
		}
		info, err := os.Stat(p)
		if err != nil || !info.IsDir() {
			return
		}
		seen[key] = true
		label := filepath.Base(p)
		if label == "." || label == string(filepath.Separator) {
			label = p
		}
		out = append(out, Choice{Value: p, Label: label})
	}
	for _, p := range extra {
		add(p)
	}
	home, err := os.UserHomeDir()
	if err != nil {
		return out
	}
	for _, name := range []string{"Desktop", "Documents", "Downloads"} {
		add(filepath.Join(home, name))
	}
	for _, name := range []string{"Projects", "Developer", "source", "code", "src", "repos"} {
		root := filepath.Join(home, name)
		info, err := os.Stat(root)
		if err != nil || !info.IsDir() {
			continue
		}
		add(root)
		entries, err := os.ReadDir(root)
		if err != nil {
			continue
		}
		for _, e := range entries {
			if !e.IsDir() || strings.HasPrefix(e.Name(), ".") || e.Name() == "node_modules" {
				continue
			}
			add(filepath.Join(root, e.Name()))
		}
	}
	return out
}
func textParts(v any) string {
	if s, ok := v.(string); ok {
		return s
	}
	out := []string{}
	for _, part := range arr(v) {
		if s, ok := part.(string); ok {
			out = append(out, s)
		} else {
			m := obj(part)
			if str(m["type"]) == "text" || str(m["type"]) == "output_text" {
				out = append(out, str(m["text"]))
			}
		}
	}
	return strings.Join(out, "\n")
}
