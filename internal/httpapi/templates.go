package httpapi

import (
	"context"
	"database/sql"
	"encoding/json"
	"errors"
	"net/http"
	"strings"
	"time"

	"gametracker/internal/formula"
)

const gameTemplateVersion = 1
const maxTemplateBytes = 1 << 20

var errTemplateLimit = errors.New("template limit reached")

type gameTemplate struct {
	ID           string            `json:"id"`
	UserID       string            `json:"-"`
	Name         string            `json:"name"`
	Description  string            `json:"description"`
	Icon         string            `json:"icon"`
	Version      int               `json:"version"`
	TypeCount    int               `json:"type_count"`
	SectionCount int               `json:"section_count"`
	CreatedAt    string            `json:"created_at"`
	UpdatedAt    string            `json:"updated_at"`
	Structure    *templateDocument `json:"structure,omitempty"`
}

type templateDocument struct {
	Version         int                  `json:"version"`
	GameIcon        string               `json:"game_icon"`
	GameDescription string               `json:"game_description"`
	Types           []templateObjectType `json:"object_types"`
	Sections        []templateSection    `json:"sections"`
}

type templateObjectType struct {
	SourceID              string          `json:"source_id"`
	Name                  string          `json:"name"`
	Description           string          `json:"description"`
	Icon                  string          `json:"icon"`
	SortOrder             int             `json:"sort_order"`
	Archived              bool            `json:"archived"`
	PrimaryDisplayFieldID string          `json:"primary_display_field_id,omitempty"`
	Fields                []templateField `json:"fields"`
}

type templateField struct {
	SourceID    string          `json:"source_id"`
	Name        string          `json:"name"`
	FieldType   string          `json:"field_type"`
	Icon        string          `json:"icon"`
	Description string          `json:"description"`
	Default     json.RawMessage `json:"default,omitempty"`
	Options     json.RawMessage `json:"options"`
	SortOrder   int             `json:"sort_order"`
	Archived    bool            `json:"archived"`
}

type templateSection struct {
	SourceID       string          `json:"source_id"`
	ParentSourceID string          `json:"parent_source_id,omitempty"`
	Name           string          `json:"name"`
	Icon           string          `json:"icon"`
	SortOrder      int             `json:"sort_order"`
	DisplayStyle   string          `json:"display_style"`
	DisplayConfig  json.RawMessage `json:"display_config"`
	Archived       bool            `json:"archived"`
}

func (a *API) createGameTemplate(w http.ResponseWriter, r *http.Request) {
	var input struct {
		Name                 string `json:"name"`
		Description          string `json:"description"`
		Icon                 string `json:"icon"`
		IncludeSections      *bool  `json:"include_sections"`
		IncludeTypes         *bool  `json:"include_types"`
		IncludeFieldDefaults *bool  `json:"include_field_defaults"`
		IncludePresentation  *bool  `json:"include_presentation"`
	}
	if !decodeJSON(w, r, &input) {
		return
	}
	input.Name = strings.TrimSpace(input.Name)
	if err := validateTemplateMetadata(input.Name, input.Description); err != nil {
		writeError(w, http.StatusBadRequest, "invalid_template", err.Error())
		return
	}
	includeSections := boolDefault(input.IncludeSections, true)
	includeTypes := boolDefault(input.IncludeTypes, true)
	includeDefaults := boolDefault(input.IncludeFieldDefaults, false)
	includePresentation := boolDefault(input.IncludePresentation, true)
	includePresentation = includePresentation && includeTypes
	userID := a.currentUser(r).ID
	tx, err := a.db.BeginTx(r.Context(), nil)
	if err != nil {
		writeError(w, http.StatusInternalServerError, "template_failed", "Could not save a template from this game.")
		return
	}
	defer tx.Rollback()
	if capacityErr := checkTemplateCapacity(r.Context(), tx, userID); errors.Is(capacityErr, errTemplateLimit) {
		writeError(w, http.StatusConflict, "template_limit", "A template library can contain at most 100 templates.")
		return
	} else if capacityErr != nil {
		writeError(w, http.StatusInternalServerError, "template_failed", "Could not validate the template library.")
		return
	}
	var document templateDocument
	var gameIcon, gameDescription string
	if err := tx.QueryRowContext(r.Context(), `SELECT icon,description FROM games WHERE id=? AND user_id=? AND archived=0`, r.PathValue("gameID"), userID).Scan(&gameIcon, &gameDescription); err != nil {
		notFoundOrError(w, err, "Game")
		return
	}
	document = templateDocument{Version: gameTemplateVersion, GameIcon: safeTemplateIcon(gameIcon, "🎮"), GameDescription: gameDescription, Types: []templateObjectType{}, Sections: []templateSection{}}
	if includeTypes {
		if err := readTemplateTypes(r.Context(), tx, r.PathValue("gameID"), includeDefaults, includePresentation, &document); err != nil {
			writeError(w, http.StatusInternalServerError, "template_failed", "Could not read this game's object types.")
			return
		}
	}
	if includeSections {
		if err := readTemplateSections(r.Context(), tx, r.PathValue("gameID"), includePresentation, &document); err != nil {
			writeError(w, http.StatusInternalServerError, "template_failed", "Could not read this game's sections.")
			return
		}
	}
	if err := validateTemplateDocument(document); err != nil {
		writeError(w, http.StatusConflict, "invalid_template_structure", "The selected game structure cannot be safely saved as a template.")
		return
	}
	encoded, err := json.Marshal(document)
	if err != nil || len(encoded) > maxTemplateBytes {
		writeError(w, http.StatusRequestEntityTooLarge, "template_too_large", "This template exceeds the 1 MB structure limit.")
		return
	}
	id, err := randomID()
	if err != nil {
		writeError(w, http.StatusInternalServerError, "template_failed", "Could not save this template.")
		return
	}
	now := time.Now().UTC().Format(time.RFC3339Nano)
	icon := safeTemplateIcon(input.Icon, "📘")
	if _, err := tx.ExecContext(r.Context(), `INSERT INTO game_templates(id,user_id,name,description,icon,template_version,data_json,created_at,updated_at) VALUES(?,?,?,?,?,?,?,?,?)`, id, userID, input.Name, input.Description, icon, gameTemplateVersion, string(encoded), now, now); err != nil {
		writeError(w, http.StatusInternalServerError, "template_failed", "Could not save this template.")
		return
	}
	if err := tx.Commit(); err != nil {
		writeError(w, http.StatusInternalServerError, "template_failed", "Could not save this template.")
		return
	}
	writeJSON(w, http.StatusCreated, templateResponse(id, input.Name, input.Description, icon, gameTemplateVersion, document, now, now, false))
}

func (a *API) importTemplate(w http.ResponseWriter, r *http.Request) {
	var input struct {
		Version     int              `json:"version"`
		Name        string           `json:"name"`
		Description string           `json:"description"`
		Icon        string           `json:"icon"`
		Structure   templateDocument `json:"structure"`
	}
	if !decodeJSON(w, r, &input) {
		return
	}
	input.Name = strings.TrimSpace(input.Name)
	if input.Version != gameTemplateVersion || input.Structure.Version != gameTemplateVersion {
		writeError(w, http.StatusBadRequest, "unsupported_template_version", "This template JSON version is not supported.")
		return
	}
	if err := validateTemplateMetadata(input.Name, input.Description); err != nil {
		writeError(w, http.StatusBadRequest, "invalid_template", err.Error())
		return
	}
	if err := validateTemplateDocument(input.Structure); err != nil {
		writeError(w, http.StatusBadRequest, "invalid_template", "Template JSON contains an invalid or unsupported structure.")
		return
	}
	encoded, err := json.Marshal(input.Structure)
	if err != nil || len(encoded) > maxTemplateBytes {
		writeError(w, http.StatusRequestEntityTooLarge, "template_too_large", "This template exceeds the 1 MB structure limit.")
		return
	}
	userID := a.currentUser(r).ID
	tx, err := a.db.BeginTx(r.Context(), nil)
	if err != nil {
		writeError(w, http.StatusInternalServerError, "template_failed", "Could not import this template.")
		return
	}
	defer tx.Rollback()
	if capacityErr := checkTemplateCapacity(r.Context(), tx, userID); capacityErr != nil {
		if errors.Is(capacityErr, errTemplateLimit) {
			writeError(w, http.StatusConflict, "template_limit", "A template library can contain at most 100 templates.")
		} else {
			writeError(w, http.StatusInternalServerError, "database_error", "Could not validate the template library.")
		}
		return
	}
	id, err := randomID()
	if err != nil {
		writeError(w, http.StatusInternalServerError, "template_failed", "Could not import this template.")
		return
	}
	now := time.Now().UTC().Format(time.RFC3339Nano)
	icon := safeTemplateIcon(input.Icon, "📘")
	if _, err := tx.ExecContext(r.Context(), `INSERT INTO game_templates(id,user_id,name,description,icon,template_version,data_json,created_at,updated_at) VALUES(?,?,?,?,?,?,?,?,?)`, id, userID, input.Name, input.Description, icon, gameTemplateVersion, string(encoded), now, now); err != nil {
		writeError(w, http.StatusInternalServerError, "template_failed", "Could not import this template.")
		return
	}
	if err := tx.Commit(); err != nil {
		writeError(w, http.StatusInternalServerError, "template_failed", "Could not import this template.")
		return
	}
	writeJSON(w, http.StatusCreated, templateResponse(id, input.Name, input.Description, icon, gameTemplateVersion, input.Structure, now, now, false))
}

func readTemplateTypes(ctx context.Context, db dbExecutor, gameID string, includeDefaults, includePresentation bool, document *templateDocument) error {
	rows, err := db.QueryContext(ctx, `SELECT id,name,description,icon,sort_order,archived FROM object_types WHERE game_id=? ORDER BY sort_order,id`, gameID)
	if err != nil {
		return err
	}
	types := []templateObjectType{}
	for rows.Next() {
		var item templateObjectType
		var archived int
		if err := rows.Scan(&item.SourceID, &item.Name, &item.Description, &item.Icon, &item.SortOrder, &archived); err != nil {
			rows.Close()
			return err
		}
		item.Icon = safeTemplateIcon(item.Icon, "◈")
		item.Archived = archived != 0
		item.Fields = []templateField{}
		types = append(types, item)
	}
	if err := rows.Err(); err != nil {
		rows.Close()
		return err
	}
	if err := rows.Close(); err != nil {
		return err
	}
	for index := range types {
		item := &types[index]
		if includePresentation {
			if err := db.QueryRowContext(ctx, `SELECT primary_display_field_id FROM object_types WHERE id=?`, item.SourceID).Scan(&item.PrimaryDisplayFieldID); err != nil {
				return err
			}
		}
		fieldRows, err := db.QueryContext(ctx, `SELECT id,name,field_type,icon,description,default_json,options_json,sort_order,archived FROM field_definitions WHERE object_type_id=? ORDER BY sort_order,id`, item.SourceID)
		if err != nil {
			return err
		}
		for fieldRows.Next() {
			var field templateField
			var defaultJSON, optionsJSON []byte
			var fieldArchived int
			if err := fieldRows.Scan(&field.SourceID, &field.Name, &field.FieldType, &field.Icon, &field.Description, &defaultJSON, &optionsJSON, &field.SortOrder, &fieldArchived); err != nil {
				fieldRows.Close()
				return err
			}
			field.Icon = safeTemplateIcon(field.Icon, "✦")
			field.Options = append(json.RawMessage(nil), optionsJSON...)
			if len(field.Options) == 0 {
				field.Options = json.RawMessage(`{}`)
			}
			if includeDefaults && len(defaultJSON) > 0 && string(defaultJSON) != "null" {
				field.Default = append(json.RawMessage(nil), defaultJSON...)
			}
			field.Archived = fieldArchived != 0
			item.Fields = append(item.Fields, field)
		}
		if err := fieldRows.Err(); err != nil {
			fieldRows.Close()
			return err
		}
		if err := fieldRows.Close(); err != nil {
			return err
		}
		document.Types = append(document.Types, *item)
	}
	return nil
}

func readTemplateSections(ctx context.Context, db dbExecutor, gameID string, includePresentation bool, document *templateDocument) error {
	rows, err := db.QueryContext(ctx, `SELECT id,parent_id,name,icon,sort_order,display_style,display_config,archived FROM sections WHERE game_id=? ORDER BY sort_order,id`, gameID)
	if err != nil {
		return err
	}
	for rows.Next() {
		var item templateSection
		var parent sql.NullString
		var displayConfig []byte
		var archived int
		if err := rows.Scan(&item.SourceID, &parent, &item.Name, &item.Icon, &item.SortOrder, &item.DisplayStyle, &displayConfig, &archived); err != nil {
			rows.Close()
			return err
		}
		item.ParentSourceID = parent.String
		item.Icon = safeTemplateIcon(item.Icon, "✦")
		item.Archived = archived != 0
		if includePresentation {
			item.DisplayConfig = append(json.RawMessage(nil), displayConfig...)
			if len(item.DisplayConfig) == 0 {
				item.DisplayConfig = json.RawMessage(`{}`)
			}
		} else {
			item.DisplayStyle = "cards"
			item.DisplayConfig = json.RawMessage(`{}`)
		}
		document.Sections = append(document.Sections, item)
	}
	if err := rows.Err(); err != nil {
		rows.Close()
		return err
	}
	return rows.Close()
}

func (a *API) listTemplates(w http.ResponseWriter, r *http.Request) {
	rows, err := a.db.QueryContext(r.Context(), `SELECT id,user_id,name,description,icon,template_version,data_json,created_at,updated_at FROM game_templates WHERE user_id=? ORDER BY updated_at DESC,id LIMIT 100`, a.currentUser(r).ID)
	if err != nil {
		writeError(w, http.StatusInternalServerError, "database_error", "Could not load game templates.")
		return
	}
	items := []gameTemplate{}
	for rows.Next() {
		item, _, err := scanTemplate(rows, false)
		if err != nil {
			rows.Close()
			writeError(w, http.StatusInternalServerError, "database_error", "Could not load game templates.")
			return
		}
		items = append(items, item)
	}
	if err := rows.Err(); err != nil {
		rows.Close()
		writeError(w, http.StatusInternalServerError, "database_error", "Could not load game templates.")
		return
	}
	rows.Close()
	writeJSON(w, http.StatusOK, map[string]any{"items": items})
}

func (a *API) getTemplate(w http.ResponseWriter, r *http.Request) {
	item, document, err := a.loadTemplate(r.Context(), a.db, a.currentUser(r).ID, r.PathValue("templateID"))
	if err != nil {
		notFoundOrError(w, err, "Template")
		return
	}
	item.Structure = &document
	writeJSON(w, http.StatusOK, item)
}

func (a *API) exportTemplate(w http.ResponseWriter, r *http.Request) {
	item, document, err := a.loadTemplate(r.Context(), a.db, a.currentUser(r).ID, r.PathValue("templateID"))
	if err != nil {
		notFoundOrError(w, err, "Template")
		return
	}
	encoded, err := json.MarshalIndent(map[string]any{"version": gameTemplateVersion, "name": item.Name, "description": item.Description, "icon": item.Icon, "structure": document}, "", "  ")
	if err != nil {
		writeError(w, http.StatusInternalServerError, "template_failed", "Could not export this template.")
		return
	}
	w.Header().Set("Content-Type", "application/json; charset=utf-8")
	w.Header().Set("Content-Disposition", `attachment; filename="game-template.json"`)
	w.WriteHeader(http.StatusOK)
	_, _ = w.Write(encoded)
}

func (a *API) updateTemplate(w http.ResponseWriter, r *http.Request) {
	var input struct {
		Name        *string `json:"name"`
		Description *string `json:"description"`
		Icon        *string `json:"icon"`
	}
	if !decodeJSON(w, r, &input) {
		return
	}
	item, document, err := a.loadTemplate(r.Context(), a.db, a.currentUser(r).ID, r.PathValue("templateID"))
	if err != nil {
		notFoundOrError(w, err, "Template")
		return
	}
	if input.Name != nil {
		item.Name = strings.TrimSpace(*input.Name)
	}
	if input.Description != nil {
		item.Description = *input.Description
	}
	if err := validateTemplateMetadata(item.Name, item.Description); err != nil {
		writeError(w, http.StatusBadRequest, "invalid_template", err.Error())
		return
	}
	if input.Icon != nil {
		item.Icon = safeTemplateIcon(*input.Icon, "📘")
	}
	item.UpdatedAt = time.Now().UTC().Format(time.RFC3339Nano)
	if _, err := a.db.ExecContext(r.Context(), `UPDATE game_templates SET name=?,description=?,icon=?,updated_at=? WHERE id=? AND user_id=?`, item.Name, item.Description, item.Icon, item.UpdatedAt, item.ID, a.currentUser(r).ID); err != nil {
		writeError(w, http.StatusInternalServerError, "template_failed", "Could not update this template.")
		return
	}
	item.Structure = &document
	writeJSON(w, http.StatusOK, item)
}

func (a *API) duplicateTemplate(w http.ResponseWriter, r *http.Request) {
	tx, err := a.db.BeginTx(r.Context(), nil)
	if err != nil {
		writeError(w, http.StatusInternalServerError, "template_failed", "Could not duplicate this template.")
		return
	}
	defer tx.Rollback()
	userID := a.currentUser(r).ID
	item, document, err := a.loadTemplate(r.Context(), tx, userID, r.PathValue("templateID"))
	if err != nil {
		notFoundOrError(w, err, "Template")
		return
	}
	if capacityErr := checkTemplateCapacity(r.Context(), tx, userID); capacityErr != nil {
		if errors.Is(capacityErr, errTemplateLimit) {
			writeError(w, http.StatusConflict, "template_limit", "A template library can contain at most 100 templates.")
		} else {
			writeError(w, http.StatusInternalServerError, "database_error", "Could not validate the template library.")
		}
		return
	}
	name := item.Name + " copy"
	if len(name) > 120 {
		name = item.Name[:115] + " copy"
	}
	encoded, err := json.Marshal(document)
	if err != nil {
		writeError(w, http.StatusInternalServerError, "template_failed", "Could not duplicate this template.")
		return
	}
	id, err := randomID()
	if err != nil {
		writeError(w, http.StatusInternalServerError, "template_failed", "Could not duplicate this template.")
		return
	}
	now := time.Now().UTC().Format(time.RFC3339Nano)
	_, err = tx.ExecContext(r.Context(), `INSERT INTO game_templates(id,user_id,name,description,icon,template_version,data_json,created_at,updated_at) VALUES(?,?,?,?,?,?,?,?,?)`, id, userID, name, item.Description, item.Icon, gameTemplateVersion, string(encoded), now, now)
	if err != nil {
		writeError(w, http.StatusInternalServerError, "template_failed", "Could not duplicate this template.")
		return
	}
	if err := tx.Commit(); err != nil {
		writeError(w, http.StatusInternalServerError, "template_failed", "Could not duplicate this template.")
		return
	}
	writeJSON(w, http.StatusCreated, templateResponse(id, name, item.Description, item.Icon, gameTemplateVersion, document, now, now, false))
}

func (a *API) deleteTemplate(w http.ResponseWriter, r *http.Request) {
	result, err := a.db.ExecContext(r.Context(), `DELETE FROM game_templates WHERE id=? AND user_id=?`, r.PathValue("templateID"), a.currentUser(r).ID)
	if err != nil {
		writeError(w, http.StatusInternalServerError, "template_failed", "Could not delete this template.")
		return
	}
	if count, _ := result.RowsAffected(); count == 0 {
		writeError(w, http.StatusNotFound, "not_found", "Template not found.")
		return
	}
	w.WriteHeader(http.StatusNoContent)
}

func (a *API) instantiateTemplate(w http.ResponseWriter, r *http.Request) {
	var input struct {
		Name string `json:"name"`
		Icon string `json:"icon"`
	}
	if !decodeJSON(w, r, &input) {
		return
	}
	input.Name = strings.TrimSpace(input.Name)
	if input.Name == "" || len(input.Name) > 120 {
		writeError(w, http.StatusBadRequest, "invalid_name", "Game name must be between 1 and 120 characters.")
		return
	}
	item, document, err := a.loadTemplate(r.Context(), a.db, a.currentUser(r).ID, r.PathValue("templateID"))
	if err != nil {
		notFoundOrError(w, err, "Template")
		return
	}
	game, err := a.instantiateTemplateData(r.Context(), a.currentUser(r).ID, item, document, input.Name, input.Icon)
	if err != nil {
		var validation validationError
		if errors.As(err, &validation) {
			writeError(w, http.StatusBadRequest, validation.code, validation.message)
		} else {
			writeError(w, http.StatusInternalServerError, "template_failed", "The game could not be created. No partial game was saved.")
		}
		return
	}
	writeJSON(w, http.StatusCreated, game)
}

func (a *API) loadTemplate(ctx context.Context, db dbExecutor, userID, id string) (gameTemplate, templateDocument, error) {
	rows, err := db.QueryContext(ctx, `SELECT id,user_id,name,description,icon,template_version,data_json,created_at,updated_at FROM game_templates WHERE id=? AND user_id=?`, id, userID)
	if err != nil {
		return gameTemplate{}, templateDocument{}, err
	}
	defer rows.Close()
	if !rows.Next() {
		if err := rows.Err(); err != nil {
			return gameTemplate{}, templateDocument{}, err
		}
		return gameTemplate{}, templateDocument{}, sql.ErrNoRows
	}
	item, data, err := scanTemplate(rows, true)
	if err != nil {
		return item, templateDocument{}, err
	}
	var document templateDocument
	if len(data) > maxTemplateBytes || json.Unmarshal(data, &document) != nil || validateTemplateDocument(document) != nil {
		return item, templateDocument{}, validationError{code: "invalid_template", message: "This template's structure is invalid or unsupported."}
	}
	return item, document, nil
}

func scanTemplate(rows *sql.Rows, includeData bool) (gameTemplate, []byte, error) {
	var item gameTemplate
	var data []byte
	if err := rows.Scan(&item.ID, &item.UserID, &item.Name, &item.Description, &item.Icon, &item.Version, &data, &item.CreatedAt, &item.UpdatedAt); err != nil {
		return item, nil, err
	}
	var document templateDocument
	if len(data) <= maxTemplateBytes && json.Unmarshal(data, &document) == nil {
		item.TypeCount, item.SectionCount = len(document.Types), len(document.Sections)
	}
	if includeData {
		return item, data, nil
	}
	return item, nil, nil
}

func templateResponse(id, name, description, icon string, version int, document templateDocument, created, updated string, includeStructure bool) gameTemplate {
	item := gameTemplate{ID: id, Name: name, Description: description, Icon: icon, Version: version, TypeCount: len(document.Types), SectionCount: len(document.Sections), CreatedAt: created, UpdatedAt: updated}
	if includeStructure {
		item.Structure = &document
	}
	return item
}

func validateTemplateMetadata(name, description string) error {
	if name == "" || len(name) > 120 {
		return errors.New("Template name must be between 1 and 120 characters.")
	}
	if len(description) > 500 {
		return errors.New("Template description must be 500 characters or fewer.")
	}
	return nil
}

func boolDefault(value *bool, fallback bool) bool {
	if value == nil {
		return fallback
	}
	return *value
}

func checkTemplateCapacity(ctx context.Context, db dbExecutor, userID string) error {
	var count int
	if err := db.QueryRowContext(ctx, `SELECT COUNT(*) FROM game_templates WHERE user_id=?`, userID).Scan(&count); err != nil {
		return err
	}
	if count >= 100 {
		return errTemplateLimit
	}
	return nil
}

func safeTemplateIcon(value, fallback string) string {
	value = strings.TrimSpace(value)
	if value == "" || len(value) > 32 || strings.ContainsAny(value, "<>/\\\"'") {
		return fallback
	}
	return value
}

func validateTemplateDocument(document templateDocument) error {
	if document.Version != gameTemplateVersion || len(document.Types) > 100 || len(document.Sections) > 1000 || len(document.GameDescription) > 300 {
		return errors.New("Template version or structure size is unsupported.")
	}
	typeIDs := map[string]bool{}
	fieldTypes := map[string]string{}
	fieldOwners := map[string]string{}
	fieldsByID := map[string]templateField{}
	formulaDependencies := map[string][]string{}
	for _, typ := range document.Types {
		if typ.SourceID == "" || len(typ.SourceID) > 128 || typeIDs[typ.SourceID] || strings.TrimSpace(typ.Name) == "" || len(typ.Name) > 80 || len(typ.Fields) > 300 {
			return errors.New("Object type definition is invalid.")
		}
		typeIDs[typ.SourceID] = true
		for _, field := range typ.Fields {
			_, duplicateField := fieldTypes[field.SourceID]
			if field.SourceID == "" || len(field.SourceID) > 128 || duplicateField || strings.TrimSpace(field.Name) == "" || len(field.Name) > 80 || !supportedFieldTypes[field.FieldType] || !json.Valid(field.Options) {
				return errors.New("Field definition is invalid.")
			}
			if err := validateFieldOptions(field.FieldType, field.Options); err != nil {
				return errors.New("Field options are invalid.")
			}
			fieldTypes[field.SourceID], fieldOwners[field.SourceID], fieldsByID[field.SourceID] = field.FieldType, typ.SourceID, field
			if len(field.Default) > 0 {
				if err := validateDefaultValue(field.FieldType, field.Options, field.Default); err != nil {
					return errors.New("Field default is invalid.")
				}
			}
		}
		if typ.PrimaryDisplayFieldID != "" {
			if fieldOwners[typ.PrimaryDisplayFieldID] != typ.SourceID {
				return errors.New("Primary display field is not part of its object type.")
			}
		}
	}
	for _, typ := range document.Types {
		for _, field := range typ.Fields {
			if field.FieldType == "reference" || field.FieldType == "references" {
				var options struct {
					AllowedTypeID string `json:"allowed_type_id"`
				}
				_ = json.Unmarshal(field.Options, &options)
				if options.AllowedTypeID != "" && !typeIDs[options.AllowedTypeID] {
					return errors.New("Reference field target type is missing from the template.")
				}
			}
			if field.FieldType != "formula" {
				continue
			}
			var config struct {
				Expression  string                   `json:"expression"`
				Aggregation *relationshipAggregation `json:"aggregation"`
			}
			if json.Unmarshal(field.Options, &config) != nil || formula.Validate(config.Expression) != nil {
				return errors.New("Formula definition is invalid.")
			}
			for _, reference := range formula.References(config.Expression) {
				if reference == "related" && config.Aggregation != nil {
					continue
				}
				if fieldOwners[reference] != typ.SourceID || (fieldTypes[reference] != "integer" && fieldTypes[reference] != "decimal" && fieldTypes[reference] != "percentage" && fieldTypes[reference] != "formula") {
					return errors.New("Formula references a missing or non-numeric field.")
				}
				if fieldTypes[reference] == "formula" {
					formulaDependencies[field.SourceID] = append(formulaDependencies[field.SourceID], reference)
				}
			}
			if config.Aggregation != nil {
				relationship := fieldsByID[config.Aggregation.RelationshipFieldID]
				if fieldOwners[relationship.SourceID] != typ.SourceID || (relationship.FieldType != "reference" && relationship.FieldType != "references") {
					return errors.New("Formula relationship is invalid.")
				}
				if config.Aggregation.Operation != "count" {
					target := fieldsByID[config.Aggregation.FieldID]
					if target.SourceID == "" || (target.FieldType != "integer" && target.FieldType != "decimal" && target.FieldType != "percentage" && target.FieldType != "progress") {
						return errors.New("Formula aggregation target is invalid.")
					}
					var relationshipOptions struct {
						AllowedTypeID string `json:"allowed_type_id"`
					}
					_ = json.Unmarshal(relationship.Options, &relationshipOptions)
					if relationshipOptions.AllowedTypeID != "" && fieldOwners[target.SourceID] != relationshipOptions.AllowedTypeID {
						return errors.New("Formula aggregate target does not match its allowed relationship type.")
					}
				} else if config.Aggregation.FieldID != "" {
					return errors.New("Count aggregation must not select a target field.")
				}
				switch config.Aggregation.Operation {
				case "sum", "avg", "min", "max", "count":
				default:
					return errors.New("Formula aggregation operation is invalid.")
				}
			}
		}
	}
	visiting, visited := map[string]bool{}, map[string]bool{}
	var visitFormula func(string) error
	visitFormula = func(id string) error {
		if visiting[id] {
			return errors.New("Formula dependencies contain a cycle.")
		}
		if visited[id] {
			return nil
		}
		visiting[id] = true
		for _, dependency := range formulaDependencies[id] {
			if err := visitFormula(dependency); err != nil {
				return err
			}
		}
		delete(visiting, id)
		visited[id] = true
		return nil
	}
	for id := range formulaDependencies {
		if err := visitFormula(id); err != nil {
			return err
		}
	}
	sectionIDs := map[string]bool{}
	for _, section := range document.Sections {
		if section.SourceID == "" || len(section.SourceID) > 128 || sectionIDs[section.SourceID] || strings.TrimSpace(section.Name) == "" || len(section.Name) > 80 || !validDisplayStyle(section.DisplayStyle) || !json.Valid(section.DisplayConfig) {
			return errors.New("Section definition is invalid.")
		}
		sectionIDs[section.SourceID] = true
	}
	for _, section := range document.Sections {
		if section.ParentSourceID != "" && !sectionIDs[section.ParentSourceID] {
			return errors.New("Section parent is missing.")
		}
		if _, err := remapTemplateJSON(section.DisplayConfig, identityMap(fieldTypes), identityMap(typeIDs)); err != nil {
			return errors.New("Section settings refer to missing template fields or types.")
		}
		seen := map[string]bool{section.SourceID: true}
		parent := section.ParentSourceID
		for parent != "" {
			if seen[parent] {
				return errors.New("Section hierarchy contains a cycle.")
			}
			seen[parent] = true
			next := ""
			for _, candidate := range document.Sections {
				if candidate.SourceID == parent {
					next = candidate.ParentSourceID
					break
				}
			}
			parent = next
		}
	}
	return nil
}

func identityMap[T any](values map[string]T) map[string]string {
	result := make(map[string]string, len(values))
	for value := range values {
		result[value] = value
	}
	return result
}
