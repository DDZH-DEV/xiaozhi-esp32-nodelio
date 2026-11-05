package bridge

import (
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"errors"
	"os"
	"path/filepath"
	"sync"
	"time"
)

type receipt struct {
	Hash    string `json:"hash"`
	Pending bool   `json:"pending"`
	Tasks   []Task `json:"tasks,omitempty"`
	Error   string `json:"error,omitempty"`
	Created int64  `json:"created"`
}
type Ledger struct {
	mu      sync.Mutex
	path    string
	entries map[string]receipt
}

func OpenLedger(dir string) (*Ledger, error) {
	l := &Ledger{path: filepath.Join(dir, "requests.json"), entries: map[string]receipt{}}
	b, err := os.ReadFile(l.path)
	if errors.Is(err, os.ErrNotExist) {
		return l, nil
	}
	if err != nil {
		return nil, err
	}
	if err = json.Unmarshal(b, &l.entries); err != nil {
		return nil, err
	}
	if l.entries == nil {
		l.entries = map[string]receipt{}
	}
	return l, nil
}
func (l *Ledger) Begin(key string, r Request) (receipt, bool, error) {
	l.mu.Lock()
	defer l.mu.Unlock()
	b, _ := json.Marshal(r)
	hash := sha256.Sum256(b)
	signature := hex.EncodeToString(hash[:])
	if old, ok := l.entries[key]; ok {
		if old.Hash != signature {
			return receipt{}, false, errors.New("request_id was already used for different parameters")
		}
		if old.Pending {
			return receipt{}, false, errors.New("request already accepted; outcome is pending or unknown, inspect task before resending")
		}
		return old, false, nil
	}
	// Keep bounded receipts for at least seven days. Never evict a possibly executed pending request.
	for k, v := range l.entries {
		if !v.Pending && v.Created < time.Now().Add(-7*24*time.Hour).Unix() {
			delete(l.entries, k)
		}
	}
	if len(l.entries) >= 4096 {
		return receipt{}, false, errors.New("request receipt limit reached; inspect/archive requests before sending more")
	}
	next := receipt{Hash: signature, Pending: true, Created: time.Now().Unix()}
	l.entries[key] = next
	if err := writeJSON(l.path, l.entries); err != nil {
		delete(l.entries, key)
		return receipt{}, false, err
	}
	return next, true, nil
}
func (l *Ledger) Finish(key string, tasks []Task, err error) error {
	l.mu.Lock()
	defer l.mu.Unlock()
	old := l.entries[key]
	next := old
	next.Pending = false
	next.Tasks = tasks
	if err != nil {
		next.Error = err.Error()
	}
	l.entries[key] = next
	if saveErr := writeJSON(l.path, l.entries); saveErr != nil {
		l.entries[key] = old
		return saveErr
	}
	return nil
}
