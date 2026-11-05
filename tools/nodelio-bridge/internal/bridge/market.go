package bridge

import (
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"strings"
)

const MarketPrefix = "/nodelio-dsh-market/"
const PackageVersion = "2.0.0"

// Immutable, embedded-in-binary packages. The exact bytes hashed here are also sent via WS.
func MarketFiles() map[string][]byte {
	files := map[string][]byte{}
	apps := []any{}
	for _, provider := range []string{"workbench", "dsh", "openai", "workbuddy"} {
		name := map[string]string{"workbench": "全部工作台", "dsh": "DSH 工作台", "openai": "OpenAI 工作台", "workbuddy": "WorkBuddy 工作台"}[provider]
		for _, op := range []string{"status", "task", "answer"} {
			app := provider + "." + op
			title := name + map[string]string{"status": "进度", "task": "派活", "answer": "确认"}[op]
			properties := map[string]any{"provider": map[string]any{"type": "string", "enum": []string{"dsh", "openai", "workbuddy"}}, "session": map[string]any{"type": "string", "maxLength": 256}, "workspace": map[string]any{"type": "string", "maxLength": 512}}
			if op == "task" {
				properties["instruction"] = map[string]any{"type": "string", "minLength": 1, "maxLength": 2000, "title": "指令", "ui_hint": "textarea"}
			}
			if op == "answer" {
				properties["notice_id"] = map[string]any{"type": "string"}
				properties["action"] = map[string]any{"type": "string"}
			}
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
			apps = append(apps, map[string]any{"app_id": app, "name": title, "version": PackageVersion, "package_url": "nodelio-dsh://" + path, "package_sha256": hex.EncodeToString(sum[:]), "summary": title, "categories": []string{"tool"}, "min_protocol": 1, "requires": doc["requires"], "optional": doc["optional"], "surfaces": doc["surfaces"]})
		}
	}
	index := map[string]any{"schema_version": 1, "market_id": "com.nodelio.dsh", "name": "NodeLio 工作台桥", "description": "DSH、OpenAI、WorkBuddy 统一连接桥", "updated_at": "2026-11-05T00:00:00Z", "publisher": map[string]any{"name": "NodeLio"}, "apps": apps}
	files[MarketPrefix+"marketplace.json"], _ = json.MarshalIndent(index, "", "  ")
	return files
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
