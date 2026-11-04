package httpapi

import (
	"context"
	"database/sql"
	"encoding/json"
	"errors"
	"net/http"
	"strings"
	"time"
)

type section struct {
	ID            string          `json:"id"`
	GameID        string          `json:"game_id"`
	ParentID      *string         `json:"parent_id"`
	Name          string          `json:"name"`
	Icon          string          `json:"icon"`
	SortOrder     int             `json:"sort_order"`
	DisplayStyle  string          `json:"display_style"`
	DisplayConfig json.RawMessage `json:"display_config"`
	Archived      bool            `json:"archived"`
	ObjectCount   int             `json:"object_count"`
}

func (a *API) listSections(w http.ResponseWriter, r *http.Request) {
	gameID := r.PathValue("gameID")
	if _, err := a.loadGame(r, gameID, false); err != nil {
		notFoundOrError(w, err, "Game")
		return
	}
	includeArchived := r.URL.Query().Get("archived") == "true"
	query := `SELECT s.id,s.game_id,s.parent_id,s.name,s.icon,s.sort_order,s.display_style,s.display_config,s.archived,
		(SELECT COUNT(*) FROM section_placements p WHERE p.section_id=s.id) FROM sections s WHERE s.game_id=?`
	if !includeArchived {
		query += ` AND s.archived=0`
	}
	query += ` ORDER BY COALESCE(s.parent_id,''),s.sort_order,s.name`
	rows, err := a.db.QueryContext(r.Context(), query, gameID)
	if err != nil {
		writeError(w, http.StatusInternalServerError, "database_error", "Could not load sections.")
		return
	}
	defer rows.Close()
	items := make([]section, 0)
	for rows.Next() {
		var item section
		var parent sql.NullString
		var displayConfig []byte
		var archived int
		if err := rows.Scan(&item.ID, &item.GameID, &parent, &item.Name, &item.Icon, &item.SortOrder, &item.DisplayStyle, &displayConfig, &archived, &item.ObjectCount); err != nil {
			writeError(w, http.StatusInternalServerError, "database_error", "Could not load sections.")
			return
		}
		if parent.Valid {
			item.ParentID = &parent.String
		}
		item.DisplayConfig = append(json.RawMessage(nil), displayConfig...)
		if len(item.DisplayConfig) == 0 {
			item.DisplayConfig = json.RawMessage(`{}`)
		}
		item.Archived = archived != 0
		items = append(items, item)
	}
	writeJSON(w, http.StatusOK, map[string]any{"items": items})
}

func (a *API) createSection(w http.ResponseWriter, r *http.Request) {
	gameID := r.PathValue("gameID")
	if _, err := a.loadGame(r, gameID, false); err != nil {
		notFoundOrError(w, err, "Game")
		return
	}
	var input struct {
		ParentID      *string         `json:"parent_id"`
		Name          string          `json:"name"`
		Icon          string          `json:"icon"`
		DisplayStyle  string          `json:"display_style"`
		DisplayConfig json.RawMessage `json:"display_config"`
	}
	if !decodeJSON(w, r, &input) {
		return
	}
	input.Name = strings.TrimSpace(input.Name)
	if input.Name == "" || len(input.Name) > 80 {
		writeError(w, http.StatusBadRequest, "invalid_name", "Section name must be between 1 and 80 characters.")
		return
	}
	if input.Icon == "" {
		input.Icon = "✦"
	}
	if input.DisplayStyle == "" {
		input.DisplayStyle = "cards"
	}
	if !validDisplayStyle(input.DisplayStyle) {
		writeError(w, http.StatusBadRequest, "invalid_display_style", "Choose cards, list, table, checklist, or progress.")
		return
	}
	if len(input.DisplayConfig) == 0 {
		input.DisplayConfig = json.RawMessage(`{}`)
	}
	if !json.Valid(input.DisplayConfig) {
		writeError(w, http.StatusBadRequest, "invalid_config", "Display configuration must be valid JSON.")
		return
	}
	if err := a.validateSectionDisplayConfig(r.Context(), gameID, input.DisplayConfig); err != nil {
		writeError(w, http.StatusBadRequest, "invalid_config", err.Error())
		return
	}
	if input.ParentID != nil {
		var exists int
		if err := a.db.QueryRowContext(r.Context(), `SELECT COUNT(*) FROM sections WHERE id=? AND game_id=? AND archived=0`, *input.ParentID, gameID).Scan(&exists); err != nil || exists == 0 {
			writeError(w, http.StatusBadRequest, "invalid_parent", "Parent section must be active and belong to this game.")
			return
		}
	}
	input.Icon = canonicalImageURL(r, input.Icon)
	if !a.validIcon(r, input.Icon) {
		writeError(w, 400, "invalid_icon", "Choose a symbol or an image you own.")
		return
	}
	var order int
	if err := a.db.QueryRowContext(r.Context(), `SELECT COALESCE(MAX(sort_order)+1,0) FROM sections WHERE game_id=? AND parent_id IS ?`, gameID, nullableString(input.ParentID)).Scan(&order); err != nil {
		writeError(w, http.StatusInternalServerError, "database_error", "Could not create section.")
		return
	}
	id, err := randomID()
	if err != nil {
		writeError(w, http.StatusInternalServerError, "create_failed", "Could not create section.")
		return
	}
	_, err = a.db.ExecContext(r.Context(), `INSERT INTO sections(id,game_id,parent_id,name,icon,sort_order,display_style,display_config,created_at,updated_at) VALUES(?,?,?,?,?,?,?,?,?,?)`, id, gameID, nullableString(input.ParentID), input.Name, input.Icon, order, input.DisplayStyle, string(input.DisplayConfig), time.Now().UTC().Format(time.RFC3339), time.Now().UTC().Format(time.RFC3339))
	if err != nil {
		writeError(w, http.StatusInternalServerError, "create_failed", "Could not create section.")
		return
	}
	writeJSON(w, http.StatusCreated, section{ID: id, GameID: gameID, ParentID: input.ParentID, Name: input.Name, Icon: input.Icon, SortOrder: order, DisplayStyle: input.DisplayStyle, DisplayConfig: input.DisplayConfig})
}

func (a *API) updateSection(w http.ResponseWriter, r *http.Request) {
	item, err := a.loadSection(r, r.PathValue("sectionID"), true)
	if err != nil {
		notFoundOrError(w, err, "Section")
		return
	}
	var input struct {
		Name          *string         `json:"name"`
		Icon          *string         `json:"icon"`
		SortOrder     *int            `json:"sort_order"`
		ParentID      json.RawMessage `json:"parent_id"`
		DisplayStyle  *string         `json:"display_style"`
		DisplayConfig json.RawMessage `json:"display_config"`
	}
	if !decodeJSON(w, r, &input) {
		return
	}
	if input.Name != nil {
		name := strings.TrimSpace(*input.Name)
		if name == "" || len(name) > 80 {
			writeError(w, http.StatusBadRequest, "invalid_name", "Section name must be between 1 and 80 characters.")
			return
		}
		item.Name = name
	}
	if input.Icon != nil {
		item.Icon = *input.Icon
	}
	item.Icon = canonicalImageURL(r, item.Icon)
	if !a.validIcon(r, item.Icon) {
		writeError(w, 400, "invalid_icon", "Choose a symbol or an image you own.")
		return
	}
	if input.SortOrder != nil {
		item.SortOrder = *input.SortOrder
	}
	if input.DisplayStyle != nil {
		if !validDisplayStyle(*input.DisplayStyle) {
			writeError(w, http.StatusBadRequest, "invalid_display_style", "Choose cards, list, table, checklist, or progress.")
			return
		}
		item.DisplayStyle = *input.DisplayStyle
	}
	if len(input.DisplayConfig) > 0 {
		if !json.Valid(input.DisplayConfig) {
			writeError(w, http.StatusBadRequest, "invalid_config", "Display configuration must be valid JSON.")
			return
		}
		item.DisplayConfig = input.DisplayConfig
	}
	if err := a.validateSectionDisplayConfig(r.Context(), item.GameID, item.DisplayConfig); err != nil {
		writeError(w, http.StatusBadRequest, "invalid_config", err.Error())
		return
	}
	if input.ParentID != nil {
		var parentID *string
		if string(input.ParentID) != "null" {
			if err := json.Unmarshal(input.ParentID, &parentID); err != nil || parentID == nil {
				writeError(w, http.StatusBadRequest, "invalid_parent", "Parent section must be an ID or null.")
				return
			}
		}
		if parentID != nil {
			if *parentID == item.ID {
				writeError(w, http.StatusBadRequest, "section_cycle", "A section cannot be its own parent.")
				return
			}
			var exists int
			if err := a.db.QueryRowContext(r.Context(), `SELECT COUNT(*) FROM sections WHERE id=? AND game_id=? AND archived=0`, *parentID, item.GameID).Scan(&exists); err != nil || exists == 0 {
				writeError(w, http.StatusBadRequest, "invalid_parent", "Parent section must belong to this game.")
				return
			}
			var cycle int
			if err := a.db.QueryRowContext(r.Context(), `WITH RECURSIVE descendants(id) AS (SELECT id FROM sections WHERE parent_id=? UNION ALL SELECT s.id FROM sections s JOIN descendants d ON s.parent_id=d.id) SELECT EXISTS(SELECT 1 FROM descendants WHERE id=?)`, item.ID, *parentID).Scan(&cycle); err != nil || cycle != 0 {
				writeError(w, http.StatusBadRequest, "section_cycle", "Moving this section would create a section cycle.")
				return
			}
		}
		item.ParentID = parentID
	}
	_, err = a.db.ExecContext(r.Context(), `UPDATE sections SET parent_id=?,name=?,icon=?,sort_order=?,display_style=?,display_config=?,updated_at=? WHERE id=?`, nullableString(item.ParentID), item.Name, item.Icon, item.SortOrder, item.DisplayStyle, string(item.DisplayConfig), time.Now().UTC().Format(time.RFC3339), item.ID)
	if err != nil {
		writeError(w, http.StatusInternalServerError, "update_failed", "Could not save section changes.")
		return
	}
	writeJSON(w, http.StatusOK, item)
}

func (a *API) archiveSection(w http.ResponseWriter, r *http.Request) {
	a.setSectionArchived(w, r, true)
}
func (a *API) restoreSection(w http.ResponseWriter, r *http.Request) {
	a.setSectionArchived(w, r, false)
}

func (a *API) setSectionArchived(w http.ResponseWriter, r *http.Request, archived bool) {
	item, err := a.loadSection(r, r.PathValue("sectionID"), true)
	if err != nil {
		notFoundOrError(w, err, "Section")
		return
	}
	_, err = a.db.ExecContext(r.Context(), `UPDATE sections SET archived=?,updated_at=? WHERE id=?`, boolInt(archived), time.Now().UTC().Format(time.RFC3339), item.ID)
	if err != nil {
		writeError(w, http.StatusInternalServerError, "update_failed", "Could not update the section.")
		return
	}
	w.WriteHeader(http.StatusNoContent)
}

func (a *API) listPlacements(w http.ResponseWriter, r *http.Request) {
	sectionID := r.PathValue("sectionID")
	if _, err := a.loadSection(r, sectionID, false); err != nil {
		notFoundOrError(w, err, "Section")
		return
	}
	rows, err := a.db.QueryContext(r.Context(), `SELECT o.id,o.name,o.object_type_id,t.name,o.description,o.image,o.archived,o.updated_at,o.created_at FROM section_placements p JOIN objects o ON o.id=p.object_id JOIN object_types t ON t.id=o.object_type_id WHERE p.section_id=? AND o.archived=0 AND t.archived=0 ORDER BY o.name`, sectionID)
	if err != nil {
		writeError(w, http.StatusInternalServerError, "database_error", "Could not load section objects.")
		return
	}
	defer rows.Close()
	items := make([]objectSummary, 0)
	for rows.Next() {
		var o objectSummary
		var archived int
		if err := rows.Scan(&o.ID, &o.Name, &o.ObjectTypeID, &o.TypeName, &o.Description, &o.Image, &archived, &o.UpdatedAt, &o.CreatedAt); err != nil {
			writeError(w, http.StatusInternalServerError, "database_error", "Could not load section objects.")
			return
		}
		items = append(items, o)
	}
	writeJSON(w, http.StatusOK, map[string]any{"items": items})
}

func (a *API) addPlacement(w http.ResponseWriter, r *http.Request) {
	sec, err := a.loadSection(r, r.PathValue("sectionID"), false)
	if err != nil {
		notFoundOrError(w, err, "Section")
		return
	}
	var input struct {
		ObjectID string `json:"object_id"`
	}
	if !decodeJSON(w, r, &input) {
		return
	}
	var exists int
	err = a.db.QueryRowContext(r.Context(), `SELECT COUNT(*) FROM objects o JOIN games g ON g.id=o.game_id JOIN object_types t ON t.id=o.object_type_id WHERE o.id=? AND o.game_id=? AND g.user_id=? AND o.archived=0 AND t.archived=0`, input.ObjectID, sec.GameID, a.currentUser(r).ID).Scan(&exists)
	if err != nil || exists == 0 {
		writeError(w, http.StatusBadRequest, "invalid_object", "Choose an active object from this game.")
		return
	}
	_, err = a.db.ExecContext(r.Context(), `INSERT OR IGNORE INTO section_placements(section_id,game_id,object_id,created_at) VALUES(?,?,?,?)`, sec.ID, sec.GameID, input.ObjectID, time.Now().UTC().Format(time.RFC3339))
	if err != nil {
		writeError(w, http.StatusInternalServerError, "placement_failed", "Could not add object to section.")
		return
	}
	w.WriteHeader(http.StatusNoContent)
}

func (a *API) removePlacement(w http.ResponseWriter, r *http.Request) {
	sec, err := a.loadSection(r, r.PathValue("sectionID"), true)
	if err != nil {
		notFoundOrError(w, err, "Section")
		return
	}
	_, err = a.db.ExecContext(r.Context(), `DELETE FROM section_placements WHERE section_id=? AND object_id=?`, sec.ID, r.PathValue("objectID"))
	if err != nil {
		writeError(w, http.StatusInternalServerError, "placement_failed", "Could not remove object from section.")
		return
	}
	w.WriteHeader(http.StatusNoContent)
}

func (a *API) loadSection(r *http.Request, id string, includeArchived bool) (section, error) {
	query := `SELECT s.id,s.game_id,s.parent_id,s.name,s.icon,s.sort_order,s.display_style,s.display_config,s.archived FROM sections s JOIN games g ON g.id=s.game_id WHERE s.id=? AND g.user_id=?`
	if !includeArchived {
		query += ` AND s.archived=0 AND g.archived=0`
	}
	var item section
	var parent sql.NullString
	var archived int
	var displayConfig []byte
	err := a.db.QueryRowContext(r.Context(), query, id, a.currentUser(r).ID).Scan(&item.ID, &item.GameID, &parent, &item.Name, &item.Icon, &item.SortOrder, &item.DisplayStyle, &displayConfig, &archived)
	if err != nil && err != sql.ErrNoRows {
		a.logger.Error("load section", "error", err)
	}
	if parent.Valid {
		item.ParentID = &parent.String
	}
	item.DisplayConfig = append(json.RawMessage(nil), displayConfig...)
	item.Archived = archived != 0
	return item, err
}

func validDisplayStyle(value string) bool {
	return value == "cards" || value == "list" || value == "table" || value == "checklist" || value == "progress"
}

func (a *API) validateSectionDisplayConfig(ctx context.Context, gameID string, raw json.RawMessage) error {
	var config struct {
		ObjectView         *objectView `json:"object_view"`
		VisibleFieldIDs    []string    `json:"visible_field_ids"`
		ChecklistField     string      `json:"checklist_field_id"`
		ProgressField      string      `json:"progress_field_id"`
		ProgressMode       string      `json:"progress_mode"`
		CurrentFields      []string    `json:"progress_current_field_ids"`
		MaximumFields      []string    `json:"progress_max_field_ids"`
		Maximum            float64     `json:"progress_maximum"`
		RelationshipField  string      `json:"progress_relationship_field_id"`
		ChildProgressField string      `json:"progress_child_field_id"`
	}
	if len(raw) == 0 {
		return nil
	}
	if err := json.Unmarshal(raw, &config); err != nil {
		return errors.New("Display configuration must be a JSON object.")
	}
	if err := a.validateObjectView(ctx, gameID, config.ObjectView); err != nil {
		return err
	}
	if len(config.VisibleFieldIDs) > 100 {
		return errors.New("Choose no more than 100 visible fields.")
	}
	seen := make(map[string]struct{}, len(config.VisibleFieldIDs))
	for _, fieldID := range config.VisibleFieldIDs {
		if _, duplicate := seen[fieldID]; duplicate {
			return errors.New("A visible field can only be selected once.")
		}
		seen[fieldID] = struct{}{}
		if _, err := a.sectionFieldType(ctx, gameID, fieldID); err != nil {
			return errors.New("Visible fields must be active fields from this game.")
		}
	}
	if config.ChecklistField != "" {
		fieldType, err := a.sectionFieldType(ctx, gameID, config.ChecklistField)
		if err != nil || fieldType != "boolean" {
			return errors.New("The checklist field must be an active checkbox field from this game.")
		}
	}
	if config.ProgressField != "" {
		fieldType, err := a.sectionFieldType(ctx, gameID, config.ProgressField)
		if err != nil || fieldType != "progress" {
			return errors.New("The progress view field must be an active progress field from this game.")
		}
	}
	if config.ProgressMode != "" && config.ProgressMode != "field" && config.ProgressMode != "calculated" && config.ProgressMode != "children" {
		return errors.New("Choose a supported progress mode.")
	}
	if config.ProgressMode == "calculated" {
		if len(config.CurrentFields) == 0 || len(config.CurrentFields) > 20 || len(config.MaximumFields) > 20 {
			return errors.New("Choose 1–20 current fields and up to 20 maximum fields.")
		}
		if len(config.MaximumFields) == 0 && config.Maximum <= 0 {
			return errors.New("Enter a positive maximum or choose maximum fields.")
		}
		typeID := ""
		seen := map[string]bool{}
		for _, id := range append(append([]string{}, config.CurrentFields...), config.MaximumFields...) {
			var kind, ownerType string
			err := a.db.QueryRowContext(ctx, `SELECT f.field_type,f.object_type_id FROM field_definitions f JOIN object_types t ON t.id=f.object_type_id WHERE f.id=? AND t.game_id=? AND f.archived=0 AND t.archived=0`, id, gameID).Scan(&kind, &ownerType)
			if err != nil || (kind != "integer" && kind != "decimal" && kind != "percentage" && kind != "formula") {
				return errors.New("Calculated progress needs active numeric fields.")
			}
			if typeID != "" && ownerType != typeID {
				return errors.New("Calculated progress fields must belong to the same object type.")
			}
			if seen[id] {
				return errors.New("Choose each progress field only once.")
			}
			seen[id], typeID = true, ownerType
		}
	}
	if config.ProgressMode == "children" {
		var typeID string
		if err := a.db.QueryRowContext(ctx, `SELECT f.object_type_id FROM field_definitions f JOIN object_types t ON t.id=f.object_type_id WHERE f.id=? AND t.game_id=? AND f.archived=0 AND t.archived=0`, config.RelationshipField, gameID).Scan(&typeID); err != nil {
			return errors.New("Choose a relationship field from this game.")
		}
		if err := a.validateAggregation(ctx, typeID, relationshipAggregation{RelationshipFieldID: config.RelationshipField, FieldID: config.ChildProgressField, Operation: "sum"}); err != nil {
			return err
		}
		kind, err := a.sectionFieldType(ctx, gameID, config.ChildProgressField)
		if err != nil || kind != "progress" {
			return errors.New("Child progress must use a current/maximum progress field.")
		}
	}
	return nil
}

func (a *API) sectionFieldType(ctx context.Context, gameID, fieldID string) (string, error) {
	var fieldType string
	err := a.db.QueryRowContext(ctx, `SELECT f.field_type FROM field_definitions f JOIN object_types t ON t.id=f.object_type_id WHERE f.id=? AND t.game_id=? AND f.archived=0`, fieldID, gameID).Scan(&fieldType)
	return fieldType, err
}

func nullableString(value *string) any {
	if value == nil {
		return nil
	}
	return *value
}
