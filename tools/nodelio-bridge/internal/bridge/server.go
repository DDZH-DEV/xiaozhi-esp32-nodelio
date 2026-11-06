package bridge

import (
	"context"
	"crypto/subtle"
	"embed"
	"encoding/json"
	"errors"
	"io"
	"net"
	"net/http"
	"os"
	"path/filepath"
	"strings"
	"time"
)

//go:embed web/*
var web embed.FS

type Server struct {
	Engine *Engine
	Host   *Host
	Dir    string
	Listen string
	Stop   func()
}

func (s *Server) Handler() http.Handler { return http.HandlerFunc(s.serve) }
func (s *Server) serve(w http.ResponseWriter, r *http.Request) {
	w.Header().Set("X-Content-Type-Options", "nosniff")
	w.Header().Set("Referrer-Policy", "no-referrer")
	w.Header().Set("Cache-Control", "no-store")
	w.Header().Set("Content-Security-Policy", "default-src 'self'; script-src 'self'; style-src 'self'; connect-src 'self'; frame-ancestors 'none'; base-uri 'none'; form-action 'self'")
	host, port, err := net.SplitHostPort(r.Host)
	_, expectedPort, _ := net.SplitHostPort(s.Listen)
	ip := net.ParseIP(host)
	if err != nil || port != expectedPort || (host != "localhost" && (ip == nil || !ip.IsLoopback())) {
		http.Error(w, "invalid Host", 403)
		return
	}
	if origin := r.Header.Get("Origin"); origin != "" && origin != "http://"+r.Host {
		http.Error(w, "cross-origin request denied", 403)
		return
	}
	if r.URL.Path == "/" || r.URL.Path == "/app.js" || r.URL.Path == "/style.css" {
		if r.Method != "GET" {
			http.Error(w, "method not allowed", 405)
			return
		}
		name := strings.TrimPrefix(r.URL.Path, "/")
		if name == "" {
			name = "index.html"
		}
		b, err := web.ReadFile("web/" + name)
		if err != nil {
			http.NotFound(w, r)
			return
		}
		w.Header().Set("Content-Type", map[string]string{"index.html": "text/html; charset=utf-8", "app.js": "text/javascript; charset=utf-8", "style.css": "text/css; charset=utf-8"}[name])
		w.Write(b)
		return
	}
	settings := s.Engine.Config.Snapshot()
	token := strings.TrimPrefix(r.Header.Get("Authorization"), "Bearer ")
	if subtle.ConstantTimeCompare([]byte(token), []byte(settings.Secrets["admin_token"])) != 1 {
		http.Error(w, "open this bridge using its local config shortcut", 401)
		return
	}
	switch r.URL.Path {
	case "/api/stop":
		if r.Method == "POST" {
			var empty struct{}
			if !decodeBody(w, r, &empty) {
				return
			}
			s.json(w, 200, map[string]bool{"ok": true})
			if s.Stop != nil {
				s.Stop()
			}
			return
		}
	case "/api/config":
		if r.Method == "GET" {
			flags := map[string]bool{}
			for _, k := range []string{"host_token", "openai_key", "workbuddy_token"} {
				flags[k] = settings.Secrets[k] != ""
			}
			s.json(w, 200, map[string]any{"config": settings.Config, "secrets_set": flags, "restart_required": settings.Config.Listen != s.Listen, "version": Version})
			return
		}
		if r.Method == "PUT" {
			var u ConfigUpdate
			if !decodeBody(w, r, &u) {
				return
			}
			if err := s.Engine.Config.Update(u); err != nil {
				s.fail(w, err)
				return
			}
			s.Host.Reload()
			s.json(w, 200, map[string]any{"ok": true, "restart_required": u.Config.Listen != s.Listen})
			return
		}
	case "/api/choices":
		if r.Method == "GET" {
			ctx, cancel := context.WithTimeout(r.Context(), 12*time.Second)
			defer cancel()
			dsh, _ := s.Engine.Providers["dsh"].(*DSH)
			oa, _ := s.Engine.Providers["openai"].(*OpenAI)
			body := map[string]any{}
			if dsh != nil {
				body["dsh"] = dsh.Choices(ctx)
			}
			if oa != nil {
				body["openai"] = oa.ModelChoices(ctx)
			}
			s.json(w, 200, body)
			return
		}
	case "/api/health":
		if r.Method == "GET" {
			ctx, cancel := context.WithTimeout(r.Context(), 6*time.Second)
			defer cancel()
			s.json(w, 200, map[string]any{"host_online": s.Host.Online(), "providers": s.Engine.Health(ctx)})
			return
		}
	case "/api/invoke":
		if r.Method == "POST" {
			var req struct {
				AppID  string  `json:"app_id"`
				Params Request `json:"params"`
			}
			if !decodeBody(w, r, &req) {
				return
			}
			ctx, cancel := context.WithTimeout(r.Context(), 22*time.Second)
			defer cancel()
			s.json(w, 200, s.Engine.Invoke(ctx, req.AppID, req.Params))
			return
		}
	case "/api/startup":
		if r.Method == "GET" {
			s.json(w, 200, StartupStatus(s.Dir))
			return
		}
		if r.Method == "PUT" {
			var req struct {
				Enabled *bool `json:"enabled"`
			}
			if !decodeBody(w, r, &req) {
				return
			}
			if req.Enabled == nil {
				s.fail(w, errors.New("enabled required"))
				return
			}
			if err := SetStartup(s.Dir, *req.Enabled); err != nil {
				s.fail(w, err)
				return
			}
			s.json(w, 200, StartupStatus(s.Dir))
			return
		}
	case "/api/logs":
		if r.Method == "GET" {
			f, err := os.Open(filepath.Join(s.Dir, "bridge.log"))
			if err != nil {
				s.json(w, 200, map[string]any{"text": "暂无日志"})
				return
			}
			defer f.Close()
			st, _ := f.Stat()
			if st.Size() > 32<<10 {
				f.Seek(-32<<10, io.SeekEnd)
			}
			b, _ := io.ReadAll(io.LimitReader(f, 32<<10))
			s.json(w, 200, map[string]any{"text": string(b)})
			return
		}
	default:
		if strings.HasPrefix(r.URL.Path, MarketPrefix) {
			if r.Method != "GET" {
				http.Error(w, "method not allowed", 405)
				return
			}
			b, ok := marketLookup(s.Host.files, r.URL.Path)
			if r.URL.Path == MarketPrefix+"marketplace.json" || r.URL.Path == MarketPrefix {
				probe, done := context.WithTimeout(r.Context(), 6*time.Second)
				b = s.Engine.MarketIndex(probe)
				done()
			}
			if !ok {
				http.NotFound(w, r)
				return
			}
			w.Header().Set("Content-Type", "application/json")
			w.Write(b)
			return
		}
		if strings.HasPrefix(r.URL.Path, "/nodelio/") && (r.Method == "GET" || r.Method == "POST") {
			parts := strings.Split(strings.TrimPrefix(r.URL.Path, "/nodelio/"), "/")
			if len(parts) != 2 {
				http.NotFound(w, r)
				return
			}
			req := Request{}
			if r.Method == "POST" {
				if !decodeBody(w, r, &req) {
					return
				}
			} else {
				if parts[1] != "status" {
					http.Error(w, "GET is read-only", 405)
					return
				}
				req.Session = r.URL.Query().Get("session")
				req.Workspace = r.URL.Query().Get("workspace")
			}
			ctx, cancel := context.WithTimeout(r.Context(), 22*time.Second)
			defer cancel()
			s.json(w, 200, s.Engine.Invoke(ctx, parts[0]+"."+parts[1], req))
			return
		}
		http.NotFound(w, r)
		return
	}
	http.Error(w, "method not allowed", 405)
}
func decodeBody(w http.ResponseWriter, r *http.Request, v any) bool {
	if !strings.HasPrefix(r.Header.Get("Content-Type"), "application/json") {
		http.Error(w, "JSON required", 415)
		return false
	}
	d := json.NewDecoder(http.MaxBytesReader(w, r.Body, 128<<10))
	d.DisallowUnknownFields()
	if err := d.Decode(v); err != nil {
		http.Error(w, "invalid JSON body", 400)
		return false
	}
	if d.Decode(new(any)) != io.EOF {
		http.Error(w, "one JSON object required", 400)
		return false
	}
	return true
}
func (*Server) json(w http.ResponseWriter, status int, v any) {
	w.Header().Set("Content-Type", "application/json; charset=utf-8")
	w.WriteHeader(status)
	_ = json.NewEncoder(w).Encode(v)
}
func (s *Server) fail(w http.ResponseWriter, err error) {
	s.json(w, 400, map[string]any{"error": err.Error()})
}
