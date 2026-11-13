package bridge

import (
	"crypto/rand"
	"encoding/hex"
	"encoding/json"
	"errors"
	"fmt"
	"net"
	"net/url"
	"os"
	"path/filepath"
	"strings"
	"sync"
)

const Version = "0.5.3"

type ProviderConfig struct {
	Enabled       bool   `json:"enabled"`
	BaseURL       string `json:"base_url,omitempty"`
	Model         string `json:"model,omitempty"`
	EndpointFile  string `json:"endpoint_file,omitempty"`
	DSHHome       string `json:"dsh_home,omitempty"`
	Workspace     string `json:"workspace,omitempty"`
	Permission    string `json:"permission,omitempty"`
	AgentPreset   string `json:"agent_preset,omitempty"`
	Mode          string `json:"mode,omitempty"`
	Executable    string `json:"executable,omitempty"`
	CodexHome     string `json:"codex_home,omitempty"`
	ControlSocket string `json:"control_socket,omitempty"`
}

type Config struct {
	SchemaVersion        int                       `json:"schema_version"`
	BridgeID             string                    `json:"bridge_id"`
	Name                 string                    `json:"name"`
	Listen               string                    `json:"listen"`
	HostWS               string                    `json:"host_ws"`
	PanelID              string                    `json:"panel_device_id"`
	DefaultProvider      string                    `json:"default_provider"`
	AllowInsecureHostWS  bool                      `json:"allow_insecure_host_ws"`
	Providers            map[string]ProviderConfig `json:"providers"`
}

// Settings is one atomic private document. The HTTP API never serializes Secrets.
// File permissions protect credentials at rest; this is not OS-keyring encryption.
type Settings struct {
	Config  Config            `json:"config"`
	Secrets map[string]string `json:"secrets"`
}

type ConfigUpdate struct {
	Config       Config            `json:"config"`
	Secrets      map[string]string `json:"secrets,omitempty"`
	ClearSecrets []string          `json:"clear_secrets,omitempty"`
}

type ConfigStore struct {
	mu  sync.RWMutex
	dir string
	s   Settings
}

func (c *ConfigStore) Dir() string {
	c.mu.RLock()
	defer c.mu.RUnlock()
	return c.dir
}

func NewID() string {
	var b [16]byte
	if _, err := rand.Read(b[:]); err != nil {
		panic(err)
	}
	return hex.EncodeToString(b[:])
}

func DefaultConfig() Config {
	name, _ := os.Hostname()
	return Config{Name: name, SchemaVersion: 1, BridgeID: "pc-" + NewID(), Listen: "127.0.0.1:8791", DefaultProvider: "dsh",
		Providers: map[string]ProviderConfig{
			"codex":     {Enabled: false, Mode: "auto"},
			"dsh":       {Enabled: false, AgentPreset: "standard"},
			"openai":    {Enabled: false, BaseURL: "https://api.openai.com/v1"},
			"workbuddy": {Enabled: false, BaseURL: "https://www.workbuddy.cn/openapi/v2", Mode: "local"},
		}}
}

func DefaultDataDir() (string, error) {
	p, err := os.UserConfigDir()
	if err != nil {
		return "", err
	}
	return filepath.Join(p, "NodeLioBridge"), nil
}

func OpenConfig(dir string) (*ConfigStore, error) {
	if err := os.MkdirAll(dir, 0700); err != nil {
		return nil, err
	}
	c := &ConfigStore{dir: dir, s: Settings{Config: DefaultConfig(), Secrets: map[string]string{}}}
	b, err := os.ReadFile(filepath.Join(dir, "settings.json"))
	if err == nil {
		if err = json.Unmarshal(b, &c.s); err != nil {
			return nil, fmt.Errorf("settings.json invalid: %w", err)
		}
		if err = ValidateConfig(c.s.Config); err != nil {
			return nil, err
		}
	} else if !errors.Is(err, os.ErrNotExist) {
		return nil, err
	}
	if c.s.Secrets == nil {
		c.s.Secrets = map[string]string{}
	}
	if c.s.Secrets["admin_token"] == "" {
		c.s.Secrets["admin_token"] = NewID() + NewID()
	}
	if err = writeJSON(filepath.Join(dir, "settings.json"), c.s); err != nil {
		return nil, err
	}
	return c, nil
}

func (s *ConfigStore) Snapshot() Settings {
	s.mu.RLock()
	defer s.mu.RUnlock()
	x := s.s
	x.Config.Providers = make(map[string]ProviderConfig, len(s.s.Config.Providers))
	x.Secrets = make(map[string]string, len(s.s.Secrets))
	for k, v := range s.s.Config.Providers {
		x.Config.Providers[k] = v
	}
	for k, v := range s.s.Secrets {
		x.Secrets[k] = v
	}
	return x
}

func (s *ConfigStore) Update(u ConfigUpdate) error {
	s.mu.Lock()
	defer s.mu.Unlock()
	u.Config.BridgeID = s.s.Config.BridgeID
	// Old management clients may still send the original three providers.
	if u.Config.Providers != nil {
		if _, ok := u.Config.Providers["codex"]; !ok {
			u.Config.Providers["codex"] = s.s.Config.Providers["codex"]
		}
	}
	if err := ValidateConfig(u.Config); err != nil {
		return err
	}
	next := Settings{Config: u.Config, Secrets: map[string]string{}}
	for k, v := range s.s.Secrets {
		next.Secrets[k] = v
	}
	for k, v := range u.Secrets {
		if k != "host_token" && k != "openai_key" && k != "workbuddy_token" {
			return fmt.Errorf("unknown secret: %s", k)
		}
		if v = strings.TrimSpace(v); v != "" {
			next.Secrets[k] = v
		}
	}
	for _, k := range u.ClearSecrets {
		if k != "host_token" && k != "openai_key" && k != "workbuddy_token" {
			return errors.New("invalid secret removal")
		}
		delete(next.Secrets, k)
	}
	if err := writeJSON(filepath.Join(s.dir, "settings.json"), next); err != nil {
		return err
	}
	s.s = next
	return nil
}

func ValidateConfig(c Config) error {
	if len([]rune(c.Name)) > 40 || strings.ContainsAny(c.Name, "\r\n\t") {
		return errors.New("电脑名称最多40个字符，不能包含换行")
	}
	if c.SchemaVersion != 1 {
		return errors.New("unsupported settings schema")
	}
	host, port, err := net.SplitHostPort(c.Listen)
	if err != nil || port == "0" || net.ParseIP(host) == nil || !net.ParseIP(host).IsLoopback() {
		return errors.New("listen must be a loopback IP and nonzero port")
	}
	if _, err = net.LookupPort("tcp", port); err != nil {
		return errors.New("invalid listen port")
	}
	if len(c.Providers) != 4 {
		return errors.New("exactly codex, dsh, openai, workbuddy providers are required")
	}
	for _, id := range []string{"codex", "dsh", "openai", "workbuddy"} {
		if _, ok := c.Providers[id]; !ok {
			return fmt.Errorf("missing provider %s", id)
		}
	}
	if _, ok := c.Providers[c.DefaultProvider]; !ok {
		return errors.New("invalid default provider")
	}
	if c.HostWS != "" {
		if err = validateURL(c.HostWS, true, c.AllowInsecureHostWS); err != nil {
			return fmt.Errorf("host_ws: %w", err)
		}
	}
	for id, p := range c.Providers {
		if id != "dsh" && id != "codex" {
			if err = validateURL(p.BaseURL, false, false); err != nil {
				return fmt.Errorf("%s base URL: %w", id, err)
			}
		}
		if id == "workbuddy" && p.Mode != "local" && p.Mode != "cloud" {
			return errors.New("workbuddy mode must be local or cloud")
		}
		if id == "codex" {
			if p.Mode != "" && p.Mode != "auto" && p.Mode != "history" {
				return errors.New("codex mode must be auto or history")
			}
			for _, path := range []string{p.Executable, p.CodexHome, p.ControlSocket} {
				if path != "" && (!filepath.IsAbs(path) || strings.ContainsAny(path, "\x00\r\n")) {
					return errors.New("Codex 程序、数据目录及套接字必须使用本机绝对路径")
				}
			}
		}
	}
	return nil
}

func validateURL(raw string, ws, allowInsecure bool) error {
	u, err := url.Parse(raw)
	if err != nil || u.Hostname() == "" || u.User != nil || u.RawQuery != "" || u.Fragment != "" {
		return errors.New("invalid endpoint URL")
	}
	secure, plain := "https", "http"
	if ws {
		secure, plain = "wss", "ws"
	}
	if u.Scheme == secure {
		return nil
	}
	if allowInsecure && u.Scheme == plain {
		return nil
	}
	ip := net.ParseIP(u.Hostname())
	if u.Scheme == plain && (u.Hostname() == "localhost" || (ip != nil && (ip.IsLoopback() || (ws && ip.IsPrivate())))) {
		return nil
	}
	return errors.New("TLS required except loopback (Host WS also permits private LAN)")
}

func writeJSON(name string, v any) error {
	b, err := json.MarshalIndent(v, "", "  ")
	if err != nil {
		return err
	}
	f, err := os.CreateTemp(filepath.Dir(name), ".bridge-*.tmp")
	if err != nil {
		return err
	}
	tmp := f.Name()
	defer os.Remove(tmp)
	if err = f.Chmod(0600); err == nil {
		_, err = f.Write(append(b, '\n'))
	}
	if err == nil {
		err = f.Sync()
	}
	closeErr := f.Close()
	if err != nil {
		return err
	}
	if closeErr != nil {
		return closeErr
	}
	return os.Rename(tmp, name)
}
