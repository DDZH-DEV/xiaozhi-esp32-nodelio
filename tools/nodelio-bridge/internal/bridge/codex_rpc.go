package bridge

import (
	"bufio"
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"os"
	"os/exec"
	"path/filepath"
	"runtime"
	"sort"
	"strings"
	"time"
)

// Use the native CLI on all platforms. No shell, database writes or auth-file parsing.
func codexExecutable(p ProviderConfig) (string, error) {
	if p.Executable != "" {
		return checkCodexExecutable(p.Executable)
	}
	if path, err := exec.LookPath("codex"); err == nil {
		if path, err = checkCodexExecutable(path); err == nil {
			return path, nil
		}
	}
	home, _ := os.UserHomeDir()
	var patterns []string
	switch runtime.GOOS {
	case "windows":
		local := os.Getenv("LOCALAPPDATA")
		if local != "" {
			patterns = append(patterns, filepath.Join(local, "OpenAI", "Codex", "bin", "*", "codex.exe"))
		}
	case "darwin":
		patterns = []string{"/Applications/Codex.app/Contents/Resources/codex", filepath.Join(home, "Applications/Codex.app/Contents/Resources/codex"), "/opt/homebrew/bin/codex", "/usr/local/bin/codex"}
	case "linux":
		patterns = []string{filepath.Join(home, ".local/bin/codex"), "/usr/local/bin/codex", "/usr/bin/codex", "/opt/Codex/resources/codex", "/opt/codex/resources/codex"}
	}
	var paths []string
	for _, pattern := range patterns {
		matches, _ := filepath.Glob(pattern)
		paths = append(paths, matches...)
	}
	// Bundled Windows CLI directories are version hashes, not sortable versions.
	sort.SliceStable(paths, func(i, j int) bool {
		a, ae := os.Stat(paths[i])
		b, be := os.Stat(paths[j])
		return ae == nil && (be != nil || a.ModTime().After(b.ModTime()))
	})
	for _, path := range paths {
		if path, err := checkCodexExecutable(path); err == nil {
			return path, nil
		}
	}
	return "", errors.New("未找到 Codex CLI；请安装本平台原生 CLI，或在高级设置指定程序路径")
}

func checkCodexExecutable(path string) (string, error) {
	info, err := os.Stat(path)
	if err != nil || !info.Mode().IsRegular() {
		return "", errors.New("Codex 程序不存在或不是普通文件")
	}
	if runtime.GOOS == "windows" && !strings.EqualFold(filepath.Ext(path), ".exe") {
		return "", errors.New("Windows 请指定原生 codex.exe，不支持 cmd/ps1 包装脚本")
	}
	if runtime.GOOS != "windows" && info.Mode().Perm()&0111 == 0 {
		return "", errors.New("Codex 程序没有执行权限")
	}
	return filepath.Abs(path)
}

func codexHome(p ProviderConfig) string {
	path := p.CodexHome
	if path == "" {
		path = os.Getenv("CODEX_HOME")
	}
	if path == "" {
		home, _ := os.UserHomeDir()
		path = filepath.Join(home, ".codex")
	}
	if absolute, err := filepath.Abs(path); err == nil {
		path = filepath.Clean(absolute)
	}
	return path
}

func codexCommand(ctx context.Context, path string, p ProviderConfig, args ...string) *exec.Cmd {
	cmd := exec.CommandContext(ctx, path, args...)
	hideCommand(cmd)
	cmd.WaitDelay = time.Second
	if p.CodexHome != "" {
		cmd.Env = []string{}
		for _, entry := range os.Environ() {
			key, _, _ := strings.Cut(entry, "=")
			if !strings.EqualFold(key, "CODEX_HOME") {
				cmd.Env = append(cmd.Env, entry)
			}
		}
		cmd.Env = append(cmd.Env, "CODEX_HOME="+p.CodexHome)
	}
	return cmd
}

// A short-lived, read-only JSONL client. Closing it only stops our proxy/reader,
// never the desktop daemon. Bounded frames and deadlines cover stalled children.
type codexReader struct {
	cmd    *exec.Cmd
	in     io.WriteCloser
	scan   *bufio.Scanner
	cancel context.CancelFunc
	id     int
	shared bool
}

func openCodexReader(ctx context.Context, path string, p ProviderConfig) (*codexReader, error) {
	if p.Mode != "history" {
		args := []string{"app-server", "proxy"}
		if p.ControlSocket != "" {
			args = append(args, "--sock", p.ControlSocket)
		}
		// A probe has its own timeout; the fallback still has the caller's budget.
		probe, cancel := context.WithCancel(ctx)
		timer := time.AfterFunc(2*time.Second, cancel)
		r, err := startCodexReader(probe, path, p, args)
		stopped := timer.Stop()
		if err == nil && stopped {
			r.shared = true
			old := r.cancel
			r.cancel = func() { old(); cancel() }
			return r, nil
		}
		if r != nil {
			r.close()
		}
		cancel()
	}
	r, err := startCodexReader(ctx, path, p, []string{"app-server", "--stdio"})
	if err != nil {
		return nil, errors.New("无法读取 Codex 桌面会话；请检查 CLI 版本与数据目录")
	}
	return r, nil
}

func startCodexReader(ctx context.Context, path string, p ProviderConfig, args []string) (*codexReader, error) {
	child, cancel := context.WithCancel(ctx)
	cmd := codexCommand(child, path, p, args...)
	in, err := cmd.StdinPipe()
	if err != nil {
		cancel()
		return nil, err
	}
	out, err := cmd.StdoutPipe()
	if err != nil {
		in.Close()
		cancel()
		return nil, err
	}
	cmd.Stderr = io.Discard // Protocol diagnostics may contain private paths/content.
	if err = cmd.Start(); err != nil {
		in.Close()
		out.Close()
		cancel()
		return nil, err
	}
	r := &codexReader{cmd: cmd, in: in, scan: bufio.NewScanner(out), cancel: cancel}
	r.scan.Buffer(make([]byte, 4096), 2<<20)
	_, err = r.call("initialize", map[string]any{"clientInfo": map[string]any{"name": "nodelio_bridge", "title": "NodeLio desktop connector", "version": Version}, "capabilities": map[string]any{"experimentalApi": true}})
	if err == nil {
		err = json.NewEncoder(in).Encode(map[string]any{"method": "initialized"})
	}
	if err != nil {
		r.close()
		return nil, err
	}
	return r, nil
}

func (r *codexReader) close() { r.in.Close(); r.cancel(); _ = r.cmd.Wait() }

func (r *codexReader) call(method string, params any) (map[string]any, error) {
	switch method {
	case "initialize", "thread/list", "thread/read", "thread/turns/list":
	default:
		return nil, errors.New("desktop history client forbids mutations")
	}
	r.id++
	if err := json.NewEncoder(r.in).Encode(map[string]any{"id": r.id, "method": method, "params": params}); err != nil {
		return nil, err
	}
	for r.scan.Scan() {
		var msg map[string]any
		if err := json.Unmarshal(r.scan.Bytes(), &msg); err != nil {
			return nil, errors.New("invalid Codex protocol frame")
		}
		if str(msg["method"]) != "" {
			// Leave approvals/tool requests to the desktop client. Even an error
			// response here could accidentally resolve another client's request.
			continue
		}
		if msg["id"] != float64(r.id) {
			continue
		}
		if msg["error"] != nil {
			return nil, fmt.Errorf("Codex %s: %s", method, clip(str(obj(msg["error"])["message"]), 200))
		}
		return obj(msg["result"]), nil
	}
	return nil, errors.New("Codex 桌面连接已中断或响应过大，请刷新后重试")
}
