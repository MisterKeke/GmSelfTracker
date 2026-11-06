package httpapi

import (
	"context"
	"database/sql"
	"encoding/json"
	"net/http"
	"strings"
	"time"
)

const dashboardVersion = 1
const maxDashboardWidgets = 20
const maxDashboardBytes = 64 << 10

type dashboardLayout struct {
	Version int               `json:"version"`
	Widgets []dashboardWidget `json:"widgets"`
}

type dashboardWidget struct {
	ID     string                     `json:"id"`
	Type   string                     `json:"type"`
	Title  string                     `json:"title"`
	Size   string                     `json:"size"`
	Config map[string]json.RawMessage `json:"config"`
}

func defaultDashboardLayout() dashboardLayout {
	return dashboardLayout{Version: dashboardVersion, Widgets: []dashboardWidget{
		{ID: "default-recent", Type: "recent_objects", Title: "Recently edited", Size: "large", Config: map[string]json.RawMessage{"limit": json.RawMessage("6")}},
		{ID: "default-stats", Type: "statistics", Title: "Game statistics", Size: "medium", Config: map[string]json.RawMessage{}},
		{ID: "default-goals", Type: "goals", Title: "Current goals", Size: "medium", Config: map[string]json.RawMessage{"limit": json.RawMessage("5"), "status": json.RawMessage(`"active"`)}},
	}}
}

func (a *API) getGameDashboard(w http.ResponseWriter, r *http.Request) {
	gameID := r.PathValue("gameID")
	if _, err := a.loadGame(r, gameID, false); err != nil {
		notFoundOrError(w, err, "Game")
		return
	}
	var raw, updatedAt string
	err := a.db.QueryRowContext(r.Context(), `SELECT layout_json,updated_at FROM game_dashboards WHERE game_id=?`, gameID).Scan(&raw, &updatedAt)
	if err == sql.ErrNoRows {
		layout, _ := json.Marshal(defaultDashboardLayout())
		writeJSON(w, http.StatusOK, map[string]any{"layout": json.RawMessage(layout), "updated_at": "", "is_default": true})
		return
	}
	if err != nil {
		writeError(w, http.StatusInternalServerError, "dashboard_failed", "Could not load the dashboard layout.")
		return
	}
	var layout dashboardLayout
	if err := json.Unmarshal([]byte(raw), &layout); err != nil {
		writeError(w, http.StatusInternalServerError, "dashboard_invalid", "The saved dashboard layout could not be loaded.")
		return
	}
	writeJSON(w, http.StatusOK, map[string]any{"layout": layout, "updated_at": updatedAt, "is_default": false})
}

func (a *API) saveGameDashboard(w http.ResponseWriter, r *http.Request) {
	gameID := r.PathValue("gameID")
	if _, err := a.loadGame(r, gameID, false); err != nil {
		notFoundOrError(w, err, "Game")
		return
	}
	var input struct {
		Layout dashboardLayout `json:"layout"`
	}
	if !decodeJSON(w, r, &input) {
		return
	}
	if err := a.validateDashboardLayout(r.Context(), gameID, input.Layout); err != nil {
		if validation, ok := err.(validationError); ok {
			writeError(w, http.StatusBadRequest, validation.code, validation.message)
		} else {
			writeError(w, http.StatusInternalServerError, "dashboard_failed", "Could not validate the dashboard layout.")
		}
		return
	}
	encoded, err := json.Marshal(input.Layout)
	if err != nil || len(encoded) > maxDashboardBytes {
		writeError(w, http.StatusRequestEntityTooLarge, "dashboard_too_large", "Dashboard layouts may be at most 64 KB.")
		return
	}
	updatedAt := time.Now().UTC().Format(time.RFC3339Nano)
	tx, err := a.db.BeginTx(r.Context(), nil)
	if err != nil {
		writeError(w, http.StatusInternalServerError, "dashboard_failed", "Could not save the dashboard layout.")
		return
	}
	defer tx.Rollback()
	if _, err := tx.ExecContext(r.Context(), `INSERT INTO game_dashboards(game_id,layout_json,updated_at) VALUES(?,?,?) ON CONFLICT(game_id) DO UPDATE SET layout_json=excluded.layout_json,updated_at=excluded.updated_at`, gameID, string(encoded), updatedAt); err != nil {
		writeError(w, http.StatusInternalServerError, "dashboard_failed", "Could not save the dashboard layout.")
		return
	}
	if _, err := tx.ExecContext(r.Context(), `DELETE FROM asset_references WHERE entity_type='dashboard' AND entity_id=?`, gameID); err != nil {
		writeError(w, http.StatusInternalServerError, "dashboard_failed", "Could not update dashboard image references.")
		return
	}
	for _, widget := range input.Layout.Widgets {
		if widget.Type != "image" {
			continue
		}
		var assetID string
		if err := json.Unmarshal(widget.Config["asset_id"], &assetID); err != nil || assetID == "" {
			continue
		}
		if _, err := tx.ExecContext(r.Context(), `INSERT INTO asset_references(entity_type,entity_id,slot,asset_id) VALUES('dashboard',?,?,?)`, gameID, widget.ID, assetID); err != nil {
			writeError(w, http.StatusInternalServerError, "dashboard_failed", "Could not update dashboard image references.")
			return
		}
	}
	if err := tx.Commit(); err != nil {
		writeError(w, http.StatusInternalServerError, "dashboard_failed", "Could not save the dashboard layout.")
		return
	}
	writeJSON(w, http.StatusOK, map[string]any{"layout": input.Layout, "updated_at": updatedAt, "is_default": false})
}

func (a *API) validateDashboardLayout(ctx context.Context, gameID string, layout dashboardLayout) error {
	if layout.Version != dashboardVersion {
		return validationError{code: "unsupported_dashboard_version", message: "This dashboard layout version is not supported."}
	}
	if len(layout.Widgets) > maxDashboardWidgets {
		return validationError{code: "too_many_widgets", message: "A dashboard can contain at most 20 widgets."}
	}
	ids := map[string]bool{}
	for index := range layout.Widgets {
		widget := &layout.Widgets[index]
		widget.ID = strings.TrimSpace(widget.ID)
		widget.Title = strings.TrimSpace(widget.Title)
		if widget.ID == "" || len(widget.ID) > 80 || ids[widget.ID] {
			return validationError{code: "invalid_widget_id", message: "Dashboard widget IDs must be unique and contain 1–80 characters."}
		}
		ids[widget.ID] = true
		if widget.Title == "" || len(widget.Title) > 80 {
			return validationError{code: "invalid_widget_title", message: "Widget titles must contain 1–80 characters."}
		}
		if widget.Size != "small" && widget.Size != "medium" && widget.Size != "large" {
			return validationError{code: "invalid_widget_size", message: "Choose a small, medium, or large widget size."}
		}
		allowed := map[string]bool{"recent_objects": true, "object_collection": true, "goals": true, "progress": true, "statistics": true, "custom_table": true, "notes": true, "image": true}
		if !allowed[widget.Type] {
			return validationError{code: "invalid_widget_type", message: "Choose a supported dashboard widget type."}
		}
		if widget.Config == nil {
			widget.Config = map[string]json.RawMessage{}
		}
		allowedKeys := map[string]bool{"type_id": true, "section_id": true, "field_ids": true, "goal_ids": true, "object_id": true, "asset_id": true, "sort": true, "direction": true, "limit": true, "status": true, "text": true}
		for key := range widget.Config {
			if !allowedKeys[key] {
				return validationError{code: "invalid_widget_config", message: "A widget contains an unsupported configuration option."}
			}
		}
		if err := a.validateDashboardWidget(ctx, gameID, *widget); err != nil {
			return err
		}
	}
	return nil
}

func (a *API) validateDashboardWidget(ctx context.Context, gameID string, widget dashboardWidget) error {
	readString := func(key string) (string, error) {
		raw, ok := widget.Config[key]
		if !ok {
			return "", nil
		}
		var value string
		if err := json.Unmarshal(raw, &value); err != nil {
			return "", validationError{code: "invalid_widget_config", message: "Widget configuration values have an invalid type."}
		}
		return value, nil
	}
	for _, key := range []string{"type_id", "section_id", "object_id", "asset_id"} {
		value, err := readString(key)
		if err != nil {
			return err
		}
		if value == "" {
			continue
		}
		var count int
		var query string
		var args []any
		switch key {
		case "type_id":
			query = `SELECT COUNT(*) FROM object_types WHERE id=? AND game_id=?`
			args = []any{value, gameID}
		case "section_id":
			query = `SELECT COUNT(*) FROM sections WHERE id=? AND game_id=?`
			args = []any{value, gameID}
		case "object_id":
			query = `SELECT COUNT(*) FROM objects WHERE id=? AND game_id=?`
			args = []any{value, gameID}
		case "asset_id":
			query = `SELECT COUNT(*) FROM uploaded_assets WHERE id=? AND user_id=(SELECT user_id FROM games WHERE id=?) AND deleted_at IS NULL`
			args = []any{value, gameID}
		}
		if err := a.db.QueryRowContext(ctx, query, args...).Scan(&count); err != nil {
			return err
		}
		if count != 1 {
			return validationError{code: "invalid_widget_source", message: "Widget sources must belong to this game or account."}
		}
	}
	typeID, _ := readString("type_id")
	for _, listKey := range []string{"field_ids", "goal_ids"} {
		raw, ok := widget.Config[listKey]
		if !ok {
			continue
		}
		var ids []string
		if err := json.Unmarshal(raw, &ids); err != nil {
			return validationError{code: "invalid_widget_config", message: "Widget ID lists must be arrays of IDs."}
		}
		if len(ids) > 100 {
			return validationError{code: "invalid_widget_config", message: "A widget may select at most 100 fields or goals."}
		}
		for _, id := range uniqueSortedStrings(ids) {
			var count int
			query := `SELECT COUNT(*) FROM field_definitions f JOIN object_types t ON t.id=f.object_type_id WHERE f.id=? AND t.game_id=?`
			args := []any{id, gameID}
			if listKey == "goal_ids" {
				query = `SELECT COUNT(*) FROM goals WHERE id=? AND game_id=?`
			} else if typeID != "" {
				query = `SELECT COUNT(*) FROM field_definitions f JOIN object_types t ON t.id=f.object_type_id WHERE f.id=? AND f.object_type_id=? AND t.game_id=?`
				args = []any{id, typeID, gameID}
			}
			if err := a.db.QueryRowContext(ctx, query, args...).Scan(&count); err != nil {
				return err
			}
			if count != 1 {
				return validationError{code: "invalid_widget_source", message: "Selected fields or goals must belong to this game and selected object type."}
			}
		}
	}
	if raw, ok := widget.Config["sort"]; ok {
		var sort string
		if json.Unmarshal(raw, &sort) != nil || sort != "updated_at" && sort != "created_at" && sort != "name" {
			return validationError{code: "invalid_widget_config", message: "Choose a supported object sort order."}
		}
	}
	if raw, ok := widget.Config["status"]; ok {
		var status string
		if err := json.Unmarshal(raw, &status); err != nil || status != "all" && status != "active" && status != "completed" {
			return validationError{code: "invalid_widget_config", message: "Goal status must be all, active, or completed."}
		}
	}
	if raw, ok := widget.Config["direction"]; ok {
		var direction string
		if err := json.Unmarshal(raw, &direction); err != nil || direction != "asc" && direction != "desc" {
			return validationError{code: "invalid_widget_config", message: "Widget sort direction must be ascending or descending."}
		}
	}
	if raw, ok := widget.Config["limit"]; ok {
		var limit int
		if err := json.Unmarshal(raw, &limit); err != nil || limit < 1 || limit > 30 {
			return validationError{code: "invalid_widget_config", message: "Widget item limits must be between 1 and 30."}
		}
	}
	if raw, ok := widget.Config["text"]; ok {
		var text string
		if err := json.Unmarshal(raw, &text); err != nil || len(text) > 4000 {
			return validationError{code: "invalid_widget_config", message: "Dashboard notes may contain up to 4,000 characters."}
		}
	}
	if widget.Type == "image" {
		asset, err := readString("asset_id")
		if err != nil || asset == "" {
			return validationError{code: "invalid_widget_source", message: "Choose an uploaded image for the image widget."}
		}
	}
	if widget.Type == "custom_table" || widget.Type == "progress" {
		if typeID == "" {
			return validationError{code: "invalid_widget_source", message: "Choose an object type for this widget so its fields have a consistent schema."}
		}
	}
	if widget.Type == "object_collection" && typeID == "" {
		sectionID, _ := readString("section_id")
		if sectionID == "" {
			return validationError{code: "invalid_widget_source", message: "Choose an object type or section for this widget."}
		}
	}
	return nil
}
