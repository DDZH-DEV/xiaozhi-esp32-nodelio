package bridge

import (
	"bytes"
	"context"
	"encoding/xml"
	"errors"
	"fmt"
	"os"
	"os/exec"
	"os/user"
	"path/filepath"
	"runtime"
	"strings"
	"time"
)

func runCommand(name string, args ...string) error {
	ctx, cancel := context.WithTimeout(context.Background(), 15*time.Second)
	defer cancel()
	c := exec.CommandContext(ctx, name, args...)
	hideCommand(c)
	if err := c.Run(); err != nil {
		return fmt.Errorf("%s failed: %w", name, err)
	}
	return nil
}
func OpenBrowser(address string) error {
	switch runtime.GOOS {
	case "windows":
		return runCommand("rundll32.exe", "url.dll,FileProtocolHandler", address)
	case "darwin":
		return runCommand("open", address)
	default:
		return runCommand("xdg-open", address)
	}
}

type Startup struct {
	Enabled   bool   `json:"enabled"`
	Supported bool   `json:"supported"`
	Mechanism string `json:"mechanism"`
	Message   string `json:"message,omitempty"`
}

func startupName(dir string) string { return "com.nodelio.bridge." + fingerprint(dir)[:12] }
func startupPath(dir string) (string, error) {
	home, err := os.UserHomeDir()
	if err != nil {
		return "", err
	}
	name := startupName(dir)
	switch runtime.GOOS {
	case "windows":
		return name, nil
	case "darwin":
		return filepath.Join(home, "Library", "LaunchAgents", name+".plist"), nil
	case "linux":
		base, err := os.UserConfigDir()
		if err != nil {
			return "", err
		}
		return filepath.Join(base, "systemd", "user", name+".service"), nil
	}
	return "", errors.New("unsupported OS")
}
func StartupStatus(dir string) Startup {
	name, err := startupPath(dir)
	if err != nil {
		return Startup{Message: err.Error()}
	}
	s := Startup{Supported: true}
	switch runtime.GOOS {
	case "windows":
		s.Mechanism = "Windows 登录计划任务"
		s.Enabled = runCommand("schtasks.exe", "/Query", "/TN", name) == nil
	case "darwin":
		s.Mechanism = "macOS LaunchAgent"
		_, err = os.Stat(name)
		s.Enabled = err == nil
	case "linux":
		s.Mechanism = "systemd 用户服务"
		if _, err = exec.LookPath("systemctl"); err != nil {
			s.Supported = false
			s.Message = "此系统没有 systemd；请使用发行版的启动管理器运行 serve"
			return s
		}
		s.Enabled = runCommand("systemctl", "--user", "is-enabled", filepath.Base(name)) == nil
	}
	return s
}
func xmlText(s string) string {
	var b bytes.Buffer
	_ = xml.EscapeText(&b, []byte(s))
	return b.String()
}
func systemdQuote(s string) string {
	return `"` + strings.NewReplacer(`\`, `\\`, `"`, `\"`, `%`, `%%`, `$`, `$$`, "\n", `\n`, "\r", `\r`).Replace(s) + `"`
}
func windowsQuote(s string) string {
	// CommandLineToArgvW quoting, including backslashes preceding a closing quote.
	var b strings.Builder
	b.WriteByte('"')
	slashes := 0
	for _, r := range s {
		if r == '\\' {
			slashes++
			continue
		}
		if r == '"' {
			b.WriteString(strings.Repeat("\\", slashes*2+1))
			b.WriteRune(r)
		} else {
			b.WriteString(strings.Repeat("\\", slashes))
			b.WriteRune(r)
		}
		slashes = 0
	}
	b.WriteString(strings.Repeat("\\", slashes*2))
	b.WriteByte('"')
	return b.String()
}
func StartupDocument(goos, exe, dir, uid string) (string, error) {
	for _, v := range []string{exe, dir, uid} {
		if strings.ContainsAny(v, "\x00\r\n") {
			return "", errors.New("invalid startup path")
		}
	}
	name := startupName(dir)
	switch goos {
	case "windows":
		return `<?xml version="1.0" encoding="UTF-8"?><Task version="1.2" xmlns="http://schemas.microsoft.com/windows/2004/02/mit/task"><RegistrationInfo><Description>NodeLio Bridge managed login task</Description></RegistrationInfo><Triggers><LogonTrigger><Enabled>true</Enabled><UserId>` + xmlText(uid) + `</UserId></LogonTrigger></Triggers><Principals><Principal id="Author"><UserId>` + xmlText(uid) + `</UserId><LogonType>InteractiveToken</LogonType><RunLevel>LeastPrivilege</RunLevel></Principal></Principals><Settings><MultipleInstancesPolicy>IgnoreNew</MultipleInstancesPolicy><DisallowStartIfOnBatteries>false</DisallowStartIfOnBatteries><StopIfGoingOnBatteries>false</StopIfGoingOnBatteries><ExecutionTimeLimit>PT0S</ExecutionTimeLimit><RestartOnFailure><Interval>PT1M</Interval><Count>3</Count></RestartOnFailure></Settings><Actions Context="Author"><Exec><Command>` + xmlText(exe) + `</Command><Arguments>serve --data-dir ` + xmlText(windowsQuote(dir)) + `</Arguments><WorkingDirectory>` + xmlText(filepath.Dir(exe)) + `</WorkingDirectory></Exec></Actions></Task>`, nil
	case "darwin":
		return `<?xml version="1.0" encoding="UTF-8"?><!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd"><plist version="1.0"><dict><key>Label</key><string>` + xmlText(name) + `</string><key>ProgramArguments</key><array><string>` + xmlText(exe) + `</string><string>serve</string><string>--data-dir</string><string>` + xmlText(dir) + `</string></array><key>RunAtLoad</key><true/><key>KeepAlive</key><dict><key>SuccessfulExit</key><false/></dict><key>ThrottleInterval</key><integer>30</integer></dict></plist>`, nil
	case "linux":
		return "# NodeLio Bridge managed login service\n[Unit]\nDescription=NodeLio Bridge\nAfter=network-online.target\n\n[Service]\nType=simple\nExecStart=" + systemdQuote(exe) + " serve --data-dir " + systemdQuote(dir) + "\nRestart=on-failure\nRestartSec=5\nUMask=0077\n\n[Install]\nWantedBy=default.target\n", nil
	}
	return "", errors.New("unsupported OS")
}
func SetStartup(dir string, enable bool) error {
	name, err := startupPath(dir)
	if err != nil {
		return err
	}
	u, err := user.Current()
	if err != nil {
		return err
	}
	if !enable {
		switch runtime.GOOS {
		case "windows":
			if !StartupStatus(dir).Enabled {
				return nil
			}
			return runCommand("schtasks.exe", "/Delete", "/TN", name, "/F")
		case "linux":
			if err = runCommand("systemctl", "--user", "disable", filepath.Base(name)); err != nil {
				return err
			}
		case "darwin": // Remove next-login registration; do not terminate the current process.
		}
		if err = os.Remove(name); err != nil && !errors.Is(err, os.ErrNotExist) {
			return err
		}
		if runtime.GOOS == "linux" {
			return runCommand("systemctl", "--user", "daemon-reload")
		}
		return nil
	}
	exe, err := os.Executable()
	if err != nil {
		return err
	}
	exe, err = filepath.EvalSymlinks(exe)
	if err != nil {
		return err
	}
	doc, err := StartupDocument(runtime.GOOS, exe, dir, u.Uid)
	if err != nil {
		return err
	}
	if runtime.GOOS == "windows" {
		f, err := os.CreateTemp(dir, "startup-*.xml")
		if err != nil {
			return err
		}
		tmp := f.Name()
		defer os.Remove(tmp)
		_, err = f.WriteString(doc)
		closeErr := f.Close()
		if err != nil {
			return err
		}
		if closeErr != nil {
			return closeErr
		}
		return runCommand("schtasks.exe", "/Create", "/TN", name, "/XML", tmp, "/F")
	}
	if err = os.MkdirAll(filepath.Dir(name), 0700); err != nil {
		return err
	}
	if err = os.WriteFile(name, []byte(doc), 0600); err != nil {
		return err
	}
	if runtime.GOOS == "linux" {
		if err = runCommand("systemctl", "--user", "daemon-reload"); err != nil {
			return err
		}
		return runCommand("systemctl", "--user", "enable", filepath.Base(name))
	}
	// LaunchAgent is loaded on next login. Avoid starting a duplicate of the active bridge.
	return nil
}
