package bridge

import (
	"context"
	"errors"
	"net/http"
	"sort"
	"strings"
)

type OpenAI struct {
	e    *Engine
	http *http.Client
}

func (p *OpenAI) Health(ctx context.Context) Health {
	s := p.e.Config.Snapshot()
	c := s.Config.Providers["openai"]
	h := Health{State: "needs_config", Message: "填写 API Key 和模型名称", Capabilities: []string{"list", "create", "continue"}}
	if s.Secrets["openai_key"] == "" || c.Model == "" {
		return h
	}
	var out any
	if err := callJSON(ctx, p.http, "GET", strings.TrimRight(c.BaseURL, "/")+"/models", s.Secrets["openai_key"], nil, &out); err != nil {
		h.State = "error"
		h.Message = err.Error()
		return h
	}
	h.State = "ready"
	h.Message = "API 已连接；独立会话，不同步 ChatGPT 历史"
	return h
}
func (p *OpenAI) ModelChoices(ctx context.Context) map[string]any {
	s := p.e.Config.Snapshot()
	c := s.Config.Providers["openai"]
	key := s.Secrets["openai_key"]
	out := map[string]any{"models": []Choice{}}
	if key == "" {
		out["error"] = "保存 API Key 后可读取模型"
		return out
	}
	var doc struct {
		Data []struct {
			ID string `json:"id"`
		} `json:"data"`
	}
	if err := callJSON(ctx, p.http, "GET", strings.TrimRight(c.BaseURL, "/")+"/models", key, nil, &doc); err != nil {
		out["error"] = err.Error()
		return out
	}
	seen := map[string]bool{}
	ids := []string{}
	for _, m := range doc.Data {
		id := strings.TrimSpace(m.ID)
		if id == "" || seen[id] {
			continue
		}
		seen[id] = true
		ids = append(ids, id)
	}
	sort.Strings(ids)
	if len(ids) > 200 {
		ids = ids[:200]
	}
	choices := make([]Choice, 0, len(ids))
	for _, id := range ids {
		choices = append(choices, Choice{Value: id, Label: id})
	}
	if len(choices) == 0 {
		out["error"] = "账号未返回模型"
	}
	out["models"] = choices
	return out
}
func (p *OpenAI) List(_ context.Context, r Request) ([]Task, error) {
	s := p.e.Config.Snapshot()
	account := fingerprint(s.Config.Providers["openai"].BaseURL, s.Secrets["openai_key"])
	out := []Task{}
	for _, t := range p.e.State.List("openai") {
		if t.Account == account && (r.Session == "" || r.Session == t.ID) {
			out = append(out, t)
		}
	}
	return out, nil
}
func (p *OpenAI) Send(_ context.Context, r Request) (Task, error) {
	s := p.e.Config.Snapshot()
	c := s.Config.Providers["openai"]
	key := s.Secrets["openai_key"]
	if key == "" || c.Model == "" {
		return Task{}, errors.New("OpenAI API key and model are required")
	}
	t, fresh, err := p.e.State.Reserve("openai", fingerprint(c.BaseURL, key), r)
	if err != nil || !fresh {
		return t, err
	}
	started := p.e.run(func(ctx context.Context) {
		t := t
		body := map[string]any{"model": c.Model, "input": r.Instruction, "store": true}
		if t.PreviousResponse != "" {
			body["previous_response_id"] = t.PreviousResponse
		}
		var out struct {
			ID     string `json:"id"`
			Status string `json:"status"`
			Output []struct {
				Type    string `json:"type"`
				Content []struct {
					Type    string `json:"type"`
					Text    string `json:"text"`
					Refusal string `json:"refusal"`
				} `json:"content"`
			} `json:"output"`
		}
		err := callJSON(ctx, p.http, "POST", strings.TrimRight(c.BaseURL, "/")+"/responses", key, body, &out)
		if err != nil {
			t.Status = "unknown"
			t.Body = err.Error() + "；请求未自动重试，请核实后新建会话。"
		} else {
			t.PreviousResponse = out.ID
			t.Status = "completed"
			if out.Status != "completed" {
				t.Status = out.Status
				if t.Status == "" {
					t.Status = "unknown"
				}
			}
			var text []string
			for _, o := range out.Output {
				for _, part := range o.Content {
					if part.Type == "output_text" {
						text = append(text, part.Text)
					}
					if part.Refusal != "" {
						text = append(text, part.Refusal)
					}
				}
			}
			t.Body = clip(strings.Join(text, "\n"), 16000)
			if t.Body == "" {
				t.Body = "模型未返回文字。此连接器暂不执行本机工具。"
			}
		}
		p.e.finish(t)
	})
	if !started {
		t.Status = "failed"
		t.Body = "桥任务队列已满，未发送"
		p.e.finish(t)
		return t, errors.New(t.Body)
	}
	return t, nil
}
func (*OpenAI) Answer(context.Context, Request) error {
	return errors.New("this OpenAI connector has no pending tool approvals")
}
