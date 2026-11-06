package bridge

import (
	"context"
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"strings"
)

const MarketPrefix = "/nodelio-dsh-market/"
const PackageVersion = "2.2.0"

// Immutable, embedded-in-binary packages. The exact bytes hashed here are also sent via WS.
func MarketFiles() map[string][]byte {
	files := map[string][]byte{}
	apps := []any{}
	for _, provider := range []string{"workbench", "dsh", "openai", "workbuddy"} {
		name := map[string]string{"workbench": "默认工作台", "dsh": "DeepSeek 工作台", "openai": "ChatGPT（独立 API）", "workbuddy": "WorkBuddy"}[provider]
		for _, op := range []string{"status", "task", "answer"} {
			app := provider + "." + op
			title := name + map[string]string{"status": "进度", "task": "派活", "answer": "确认"}[op]
			properties := map[string]any{"provider": map[string]any{"type": "string", "enum": []string{"dsh", "openai", "workbuddy"}}, "session": map[string]any{"type": "string", "maxLength": 256}, "workspace": map[string]any{"type": "string", "maxLength": 512}}
			if op == "status" {
				title = name
				properties["operation"] = map[string]any{"type": "string", "enum": []string{"status", "task", "answer"}}
				properties["instruction"] = map[string]any{"type": "string", "maxLength": 2000}
				properties["notice_id"] = map[string]any{"type": "string"}
				properties["action"] = map[string]any{"type": "string"}
				properties["request_id"] = map[string]any{"type": "string", "maxLength": 128}
			}
			if op == "task" {
				properties["instruction"] = map[string]any{"type": "string", "minLength": 1, "maxLength": 2000, "title": "指令", "ui_hint": "textarea"}
			}
			if op == "answer" {
				properties["notice_id"] = map[string]any{"type": "string"}
				properties["action"] = map[string]any{"type": "string"}
			}
			properties["bridge_id"] = map[string]any{"type": "string", "maxLength": 63}
			properties["computer"] = map[string]any{"type": "string", "maxLength": 40}
			properties["select_computer"] = map[string]any{"type": "boolean"}
			properties["computer_page"] = map[string]any{"type": "integer", "minimum": 0, "maximum": 4}
			input := map[string]any{"type": "object", "properties": properties, "required": []string{}}
			if op == "task" {
				input["required"] = []string{"instruction"}
			}
			doc := map[string]any{"schema_version": 1, "app_id": app, "version": PackageVersion, "name": title, "description": "由 NodeLio Bridge 连接工作台；先在电脑桥中启用并完成配置。", "publisher": map[string]any{"name": "NodeLio"}, "categories": []string{"tool"}, "min_protocol": 1, "requires": []string{"content.list", "action.invoke", "net.http_via_server"}, "optional": []string{"content.text", "content.error", "content.action"}, "surfaces": []string{"card"}, "fallback": map[string]any{"card_type": "plain_result"}, "config_schema": map[string]any{"type": "object", "properties": map[string]any{}}, "input_schema": input,
				"invoke": map[string]any{"method": "POST", "timeout_ms": 22000, "transport": "outbound_bridge", "url": "http://127.0.0.1:8791/nodelio/" + provider + "/" + op, "result_types": []string{"list", "plain_result", "error"}, "default_display": map[string]any{"card_type": "list", "preferred_surface": "card"}}, "expose_mcp": op != "answer", "mcp": map[string]any{"name": app, "description": title}, "ui": map[string]any{"entry_title": title, "entry_icon": "terminal"}}
			b, _ := json.MarshalIndent(doc, "", "  ")
			path := "packages/" + app + "/" + PackageVersion + ".json"
			files[MarketPrefix+path] = b
			sum := sha256.Sum256(b)
			// Each status app owns task and approval actions. Legacy helper packages remain fetchable.
			if op == "status" {
				apps = append(apps, map[string]any{"app_id": app, "name": title, "version": PackageVersion, "package_url": "nodelio-dsh://" + path, "package_sha256": hex.EncodeToString(sum[:]), "summary": "查看任务、开始指令和续聊；连接及凭证在电脑桥配置", "categories": []string{"tool"}, "min_protocol": 1, "requires": doc["requires"], "optional": doc["optional"], "surfaces": doc["surfaces"]})
			}
		}
	}
	index := map[string]any{"schema_version": 1, "market_id": "com.nodelio.dsh", "name": "NodeLio 工作台桥", "description": "DSH、OpenAI、WorkBuddy 统一连接桥", "updated_at": "2026-11-05T00:00:00Z", "publisher": map[string]any{"name": "NodeLio"}, "apps": apps}
	files[MarketPrefix+"marketplace.json"], _ = json.MarshalIndent(index, "", "  ")
	return files
}

// Only the index carries live availability; package bytes and hashes stay immutable.
func (e *Engine) MarketIndex(ctx context.Context) []byte {
	var doc map[string]any
	_ = json.Unmarshal(MarketFiles()[MarketPrefix+"marketplace.json"], &doc)
	states := e.Health(ctx)
	cfg := e.Config.Snapshot().Config
	for _, item := range arr(doc["apps"]) {
		app := obj(item)
		id := strings.SplitN(str(app["app_id"]), ".", 2)[0]
		if id == "workbench" {
			id = cfg.DefaultProvider
		}
		app["availability"] = states[id]
	}
	doc["providers"] = states
	doc["bridge_id"] = cfg.BridgeID
	doc["bridge_version"] = Version
	b, _ := json.Marshal(doc)
	return b
}

func marketLookup(files map[string][]byte, path string) ([]byte, bool) {
	if strings.Contains(path, "..") || strings.ContainsAny(path, "\\\x00") {
		return nil, false
	}
	if path == strings.TrimSuffix(MarketPrefix, "/") || path == MarketPrefix {
		path = MarketPrefix + "marketplace.json"
	}
	b, ok := files[path]
	return b, ok
}
