package bridge

import (
	"context"
	"encoding/base64"
	"encoding/json"
	"errors"
	"strings"
)

type Request struct {
	Operation        string         `json:"operation,omitempty"`
	Arm              bool           `json:"arm,omitempty"`
	RequestID        string         `json:"request_id,omitempty"`
	Provider         string         `json:"provider,omitempty"`
	Session          string         `json:"session,omitempty"`
	Instruction      string         `json:"instruction,omitempty"`
	Workspace        string         `json:"workspace,omitempty"`
	DefaultWorkspace string         `json:"default_workspace,omitempty"`
	NewSession       bool           `json:"new_session,omitempty"`
	NoticeID         string         `json:"notice_id,omitempty"`
	Action           string         `json:"action,omitempty"`
	Answers          map[string]any `json:"answers,omitempty"`
}
type Task struct {
	ID               string   `json:"id"`
	Provider         string   `json:"provider"`
	Title            string   `json:"title"`
	Status           string   `json:"status"`
	Body             string   `json:"body,omitempty"`
	Workspace        string   `json:"workspace,omitempty"`
	Notices          []Notice `json:"notices,omitempty"`
	Updated          int64    `json:"updated"`
	RemoteID         string   `json:"remote_id,omitempty"`
	PreviousResponse string   `json:"previous_response,omitempty"`
	Account          string   `json:"account,omitempty"`
	RequestID        string   `json:"request_id,omitempty"`
}
type Notice struct {
	ID      string          `json:"id"`
	Title   string          `json:"title"`
	Options []Option        `json:"options"`
	Raw     json.RawMessage `json:"raw,omitempty"`
}
type Option struct {
	ID    string `json:"id"`
	Title string `json:"title"`
}
type Choice struct {
	Value string `json:"value"`
	Label string `json:"label"`
}
type Health struct {
	State        string   `json:"state"`
	Message      string   `json:"message"`
	Capabilities []string `json:"capabilities"`
}
type Provider interface {
	Health(context.Context) Health
	List(context.Context, Request) ([]Task, error)
	Send(context.Context, Request) (Task, error)
	Answer(context.Context, Request) error
}

func sessionKey(provider, id string) string {
	return provider + ":" + base64.RawURLEncoding.EncodeToString([]byte(id))
}
func parseSession(key string) (provider, id string, err error) {
	p := strings.SplitN(key, ":", 2)
	if len(p) != 2 {
		return "", "", errors.New("session must include provider identity")
	}
	b, e := base64.RawURLEncoding.DecodeString(p[1])
	if e != nil || len(b) == 0 {
		return "", "", errors.New("invalid session identity")
	}
	return p[0], string(b), nil
}
func clip(s string, n int) string {
	r := []rune(s)
	if len(r) > n {
		return string(r[:n]) + "…"
	}
	return s
}
func obj(v any) map[string]any {
	m, _ := v.(map[string]any)
	if m == nil {
		return map[string]any{}
	}
	return m
}
func str(v any) string { s, _ := v.(string); return s }
func arr(v any) []any  { a, _ := v.([]any); return a }
