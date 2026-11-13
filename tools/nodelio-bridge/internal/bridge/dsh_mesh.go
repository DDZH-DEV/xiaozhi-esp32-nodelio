package bridge

import (
	"context"
	"crypto/rand"
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"errors"
	"fmt"
	"os"
	"os/exec"
	"path/filepath"
	"runtime"
	"sync"
	"time"

	meshcall "github.com/t4wefan/meshcall/meshcall-go"
)

const (
	dshBridgeService   = "aa.dsh.v2.Bridge"
	dshSessionNS       = "nodelio-bridge"
	dshMetaNamespace   = "aa-session-namespace"
	dshMetaConnectorID = "aa-connector-id"
)

// dshMesh owns a local MeshCall Router so DSH NEXT can register aa.dsh.v2.Bridge
// on our pipe, and we call it with the connector-role key (protocol 2.x).
type dshMesh struct {
	mu         sync.Mutex
	router     *meshcall.LocalRouter
	mesh       *meshcall.Mesh
	authFile   string
	pipe       string
	instanceID string
	pid        int
	connKey    string
	bridgeKey  string
	rendezvous string
	binary     string
}

var (
	sharedDSHMesh     *dshMesh
	sharedDSHMeshOnce sync.Once
)

func getDSHMesh() *dshMesh {
	sharedDSHMeshOnce.Do(func() { sharedDSHMesh = &dshMesh{} })
	return sharedDSHMesh
}

func (m *dshMesh) ensure(ctx context.Context, dataDir string) error {
	m.mu.Lock()
	defer m.mu.Unlock()
	if m.mesh != nil && m.router != nil && m.router.IsRunning() {
		return m.waitBridge(ctx)
	}
	bin, err := findMeshcallRouter()
	if err != nil {
		return err
	}
	m.binary = bin
	home, err := os.UserHomeDir()
	if err != nil {
		return err
	}
	bridgeDir := filepath.Join(home, ".agents-anywhere", "dsh-bridge")
	if err = os.MkdirAll(bridgeDir, 0700); err != nil {
		return err
	}
	m.rendezvous = filepath.Join(bridgeDir, "connector.json")
	work := filepath.Join(dataDir, "dsh-mesh")
	if err = os.MkdirAll(work, 0700); err != nil {
		return err
	}
	m.authFile = filepath.Join(work, "router.auth.json")
	m.instanceID = "nl" + NewID()[:14]
	m.pipe = "nodelio-dsh-" + m.instanceID
	m.connKey = newMeshAPIKey("connector")
	m.bridgeKey = newMeshAPIKey("dsh-bridge")
	if err = writeMeshAuthFile(m.authFile, m.connKey, m.bridgeKey); err != nil {
		return err
	}
	startCtx, cancel := context.WithTimeout(ctx, 20*time.Second)
	defer cancel()
	router, err := meshcall.StartLocalRouter(startCtx,
		meshcall.WithPipe(m.pipe),
		meshcall.WithAuthFile(m.authFile),
		meshcall.WithRouterBinary(m.binary),
	)
	if err != nil {
		return fmt.Errorf("start meshcall-router: %w", err)
	}
	m.router = router
	m.pid = os.Getpid()
	mesh := meshcall.NewMesh(router, meshcall.WithToken(m.connKey), meshcall.WithPeerID("nodelio-"+m.instanceID))
	if err = mesh.Connect(startCtx); err != nil {
		_ = router.Stop()
		m.router = nil
		return fmt.Errorf("connect mesh: %w", err)
	}
	m.mesh = mesh
	if err = m.publishRendezvous(); err != nil {
		m.teardownLocked()
		return err
	}
	return m.waitBridge(ctx)
}

func (m *dshMesh) publishRendezvous() error {
	doc := map[string]any{
		"version":    2,
		"protocol":   "meshcall",
		"instanceId": m.instanceID,
		"pid":        m.pid,
		"endpoint":   map[string]string{"pipe": m.pipe},
		"key":        m.bridgeKey,
	}
	b, err := json.MarshalIndent(doc, "", "  ")
	if err != nil {
		return err
	}
	tmp := m.rendezvous + "." + NewID()[:8] + ".tmp"
	if err = os.WriteFile(tmp, append(b, '\n'), 0600); err != nil {
		return err
	}
	return os.Rename(tmp, m.rendezvous)
}

func (m *dshMesh) waitBridge(ctx context.Context) error {
	deadline := time.Now().Add(8 * time.Second)
	if d, ok := ctx.Deadline(); ok && d.Before(deadline) {
		deadline = d
	}
	var last error
	for time.Now().Before(deadline) {
		callCtx, cancel := context.WithTimeout(ctx, 2*time.Second)
		var out map[string]any
		err := m.call(callCtx, "initialize", map[string]any{
			"protocolVersion": "2.0",
			"runtime":         "dsh",
			"clientInfo":      map[string]any{"name": "nodelio-bridge", "version": Version},
		}, &out)
		cancel()
		if err == nil {
			identity := obj(out["identity"])
			if str(identity["runtime"]) == "dsh" && stringsHasPrefix(str(identity["protocolVersion"]), "2.") {
				return nil
			}
			last = errors.New("DSH bridge protocol is incompatible")
		} else {
			last = err
		}
		select {
		case <-ctx.Done():
			return ctx.Err()
		case <-time.After(250 * time.Millisecond):
		}
	}
	if last == nil {
		last = errors.New("DSH bridge not registered")
	}
	return fmt.Errorf("%w; open DSH NEXT and keep it running (Agents Anywhere runtime)", last)
}

func (m *dshMesh) call(ctx context.Context, method string, params any, out any) error {
	if m.mesh == nil {
		return errors.New("DSH mesh not ready")
	}
	raw, err := json.Marshal(params)
	if err != nil {
		return err
	}
	if len(raw) == 0 || string(raw) == "null" {
		raw = []byte("{}")
	}
	opts := []meshcall.CallOption{
		meshcall.WithTimeout(20 * time.Second),
		meshcall.WithMetadata(map[string]string{
			dshMetaNamespace:   dshSessionNS,
			dshMetaConnectorID: "nodelio-bridge",
		}),
		meshcall.IdempotentCall(),
	}
	if method == "initialize" || method == "ping" || method == "listSessions" || method == "getSessionState" || method == "getSessionNotices" || method == "listModels" || method == "listPermissions" || method == "listAgentPresets" {
		// already IdempotentCall
	} else {
		opts = []meshcall.CallOption{
			meshcall.WithTimeout(20 * time.Second),
			meshcall.WithMetadata(map[string]string{
				dshMetaNamespace:   dshSessionNS,
				dshMetaConnectorID: "nodelio-bridge",
			}),
		}
	}
	type payload = map[string]any
	var req payload
	if err = json.Unmarshal(raw, &req); err != nil {
		return err
	}
	resp, err := meshcall.Unary[payload, payload](ctx, m.mesh.Client(), dshBridgeService, method, req, opts...)
	if err != nil {
		return fmt.Errorf("DSH %s: %v", method, err)
	}
	if out == nil {
		return nil
	}
	b, err := json.Marshal(resp)
	if err != nil {
		return err
	}
	return json.Unmarshal(b, out)
}

func (m *dshMesh) teardownLocked() {
	if m.rendezvous != "" {
		b, err := os.ReadFile(m.rendezvous)
		if err == nil {
			var doc map[string]any
			if json.Unmarshal(b, &doc) == nil && str(doc["instanceId"]) == m.instanceID {
				_ = os.Remove(m.rendezvous)
			}
		}
	}
	if m.mesh != nil {
		_ = m.mesh.Close()
		m.mesh = nil
	}
	if m.router != nil {
		_ = m.router.Stop()
		m.router = nil
	}
}

func (m *dshMesh) Close() {
	m.mu.Lock()
	defer m.mu.Unlock()
	m.teardownLocked()
}

// CloseDSHMesh stops the shared MeshCall router and removes our connector.json.
func CloseDSHMesh() {
	if sharedDSHMesh != nil {
		sharedDSHMesh.Close()
	}
}

func newMeshAPIKey(id string) string {
	var b [32]byte
	if _, err := rand.Read(b[:]); err != nil {
		panic(err)
	}
	return "mk_" + id + "_" + hex.EncodeToString(b[:])
}

func meshKeyHash(key string) string {
	sum := sha256.Sum256([]byte(key))
	return hex.EncodeToString(sum[:])
}

func writeMeshAuthFile(path, connectorKey, bridgeKey string) error {
	// Match Agents Anywhere LocalHub ROLE_PERMISSIONS so DSH can register Bridge.
	doc := map[string]any{
		"api_keys": []map[string]any{
			{
				"id":       "connector",
				"key_hash": meshKeyHash(connectorKey),
				"roles":    []string{"client", "server"},
				"register": []string{"aa.local.v2.Control", "aa.local.v2.Logs", "aa.dsh.v2.Sync"},
				"call":     []string{dshBridgeService + "/*", "meshcall.router.v1.Registry/*"},
			},
			{
				"id":       "dsh-bridge",
				"key_hash": meshKeyHash(bridgeKey),
				"roles":    []string{"client", "server"},
				"register": []string{dshBridgeService},
				"call":     []string{"aa.dsh.v2.Sync/*", "aa.local.v2.Logs/write", "aa.local.v2.Logs/read", "aa.local.v2.Logs/tail"},
			},
		},
	}
	b, err := json.MarshalIndent(doc, "", "  ")
	if err != nil {
		return err
	}
	tmp := path + ".tmp"
	if err = os.WriteFile(tmp, append(b, '\n'), 0600); err != nil {
		return err
	}
	return os.Rename(tmp, path)
}

func findMeshcallRouter() (string, error) {
	if env := os.Getenv("MESHCALL_ROUTER_BINARY"); env != "" {
		if st, err := os.Stat(env); err == nil && !st.IsDir() {
			return env, nil
		}
		return "", fmt.Errorf("MESHCALL_ROUTER_BINARY not found: %s", env)
	}
	name := "meshcall-router"
	if runtime.GOOS == "windows" {
		name += ".exe"
	}
	tag := runtime.GOOS + "-" + runtime.GOARCH
	candidates := []string{}
	if exe, err := os.Executable(); err == nil {
		dir := filepath.Dir(exe)
		candidates = append(candidates,
			filepath.Join(dir, name),
			filepath.Join(dir, "meshcall-router", name),
			filepath.Join(dir, "packaging", "meshcall-router", tag, name),
		)
	}
	if cwd, err := os.Getwd(); err == nil {
		candidates = append(candidates,
			filepath.Join(cwd, name),
			filepath.Join(cwd, "packaging", "meshcall-router", tag, name),
		)
	}
	for _, candidate := range candidates {
		if st, err := os.Stat(candidate); err == nil && !st.IsDir() {
			return candidate, nil
		}
	}
	if path, err := exec.LookPath(name); err == nil {
		return path, nil
	}
	return "", errors.New("meshcall-router not found; place it next to nodelio-bridge or set MESHCALL_ROUTER_BINARY")
}

func stringsHasPrefix(s, prefix string) bool {
	return len(s) >= len(prefix) && s[:len(prefix)] == prefix
}

func dshMethodV2(method string) string {
	switch method {
	case "initialize":
		return "initialize"
	case "session.list":
		return "listSessions"
	case "session.getState":
		return "getSessionState"
	case "session.getNotices":
		return "getSessionNotices"
	case "session.startTurn":
		return "startTurn"
	case "session.createAndStart":
		return "createAndStartSession"
	case "session.respondInteraction":
		return "respondInteraction"
	case "session.interrupt":
		return "interruptSession"
	case "catalog.listModels":
		return "listModels"
	case "catalog.listPermissions":
		return "listPermissions"
	case "catalog.listAgentPresets":
		return "listAgentPresets"
	default:
		return method
	}
}
