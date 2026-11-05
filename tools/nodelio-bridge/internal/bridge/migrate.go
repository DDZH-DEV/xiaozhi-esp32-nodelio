package bridge

import (
	"encoding/json"
	"errors"
	"os"
)

// ImportLegacy is explicit and read-only toward the old bridge. Existing enabled
// connectors must not be overwritten by an accidental second import.
func (s *ConfigStore) ImportLegacy(name string) error {
	settings := s.Snapshot()
	for _, p := range settings.Config.Providers {
		if p.Enabled {
			return errors.New("legacy import requires all new bridge connectors to be disabled")
		}
	}
	b, err := os.ReadFile(name)
	if err != nil {
		return err
	}
	var old struct {
		HostWS     string `json:"host_ws"`
		Token      string `json:"bridge_token"`
		PanelID    string `json:"panel_device_id"`
		Model      string `json:"default_model"`
		Permission string `json:"default_permission"`
		Agent      string `json:"default_agent_preset"`
	}
	if err = json.Unmarshal(b, &old); err != nil {
		return err
	}
	settings.Config.HostWS = old.HostWS
	settings.Config.PanelID = old.PanelID
	p := settings.Config.Providers["dsh"]
	p.Enabled = true
	p.Model = old.Model
	p.Permission = old.Permission
	if old.Agent != "" {
		p.AgentPreset = old.Agent
	}
	settings.Config.Providers["dsh"] = p
	return s.Update(ConfigUpdate{Config: settings.Config, Secrets: map[string]string{"host_token": old.Token}})
}
