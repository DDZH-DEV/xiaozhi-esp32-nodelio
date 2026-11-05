package bridge

import (
	"bufio"
	"encoding/json"
	"github.com/klauspost/compress/zstd"
	"io"
	"os"
	"path/filepath"
	"regexp"
	"strings"
	"sync"
)

type logEntry struct {
	mtime             int64
	size              int64
	Title, User, Body string
}
type dshLogs struct {
	mu      sync.Mutex
	entries map[string]logEntry
}

var slugPattern = regexp.MustCompile(`[\\/:]+`)

func (l *dshLogs) read(home, cwd, id string) logEntry {
	if id == "" || strings.ContainsAny(id, "/\\") || id == ".." {
		return logEntry{}
	}
	name := filepath.Join(home, "sessions", "--"+slugPattern.ReplaceAllString(cwd, "-")+"--", id, "session.v4.jsonl.zstd")
	st, err := os.Stat(name)
	if err != nil || st.Size() > 32<<20 {
		return logEntry{}
	}
	l.mu.Lock()
	defer l.mu.Unlock()
	cached, ok := l.entries[name]
	if ok && cached.mtime == st.ModTime().UnixNano() && cached.size == st.Size() {
		return cached
	}
	f, err := os.Open(name)
	if err != nil {
		return logEntry{}
	}
	defer f.Close()
	dec, err := zstd.NewReader(f, zstd.WithDecoderConcurrency(1), zstd.WithDecoderMaxMemory(64<<20))
	if err != nil {
		return logEntry{}
	}
	defer dec.Close()
	entry := logEntry{mtime: st.ModTime().UnixNano(), size: st.Size()}
	scanner := bufio.NewScanner(io.LimitReader(dec, 64<<20))
	scanner.Buffer(make([]byte, 4096), 2<<20)
	for scanner.Scan() {
		var ev map[string]any
		if json.Unmarshal(scanner.Bytes(), &ev) != nil {
			continue
		}
		d := obj(ev["data"])
		switch str(ev["type"]) {
		case "session/title":
			entry.Title = str(d["title"])
		case "user/message":
			kind := str(obj(d["source"])["kind"])
			if kind != "" && kind != "user" {
				continue
			}
			v := d["content"]
			if v == nil {
				v = obj(d["message"])["content"]
			}
			entry.User = clip(textParts(v), 96)
		case "assistant/message":
			v := obj(d["message"])["content"]
			if v == nil {
				v = d["content"]
			}
			if text := textParts(v); text != "" {
				entry.Body = clip(text, 16000)
			}
		}
	}
	// A writer may leave a partial final frame: keep complete events; retry after mtime changes.
	if len(l.entries) >= 32 {
		l.entries = map[string]logEntry{}
	}
	l.entries[name] = entry
	return entry
}
