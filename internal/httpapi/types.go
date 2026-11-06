package httpapi

import (
	"context"
	"encoding/json"
	"fmt"
	"net/http"
	"sort"
	"strings"
	"time"

	"gametracker/internal/formula"
)

var supportedFieldTypes = map[string]bool{
	"text": true, "long_text": true, "integer": true, "decimal": true, "percentage": true, "boolean": true,
	"select": true, "date": true, "progress": true, "reference": true, "references": true, "formula": true,
}

type fieldDefinition struct {
	ID           string          `json:"id"`
	ObjectTypeID string          `json:"object_type_id"`
	Name         string          `json:"name"`
	FieldType    string          `json:"field_type"`
	Icon         string          `json:"icon"`
	Description  string          `json:"description"`
	DefaultValue json.RawMessage `json:"default_value,omitempty"`
	Options      json.RawMessage `json:"options"`
	SortOrder    int             `json:"sort_order"`
	Archived     bool            `json:"archived"`
}

type objectType struct {
	PrimaryDisplayFieldID string            `json:"primary_display_field_id"`
	Archived              bool              `json:"archived"`
	ID                    string            `json:"id"`
	GameID                string            `json:"game_id"`
	Name                  string            `json:"name"`
	Description           string            `json:"description"`
	Icon                  string            `json:"icon"`
	Image                 string            `json:"image"`
	SortOrder             int               `json:"sort_order"`
	Fields                []fieldDefinition `json:"fields"`
}

type optionalRawMessage struct {
	Present bool
	Value   json.RawMessage
}

func (value *optionalRawMessage) UnmarshalJSON(raw []byte) error {
	value.Present = true
	value.Value = append(json.RawMessage(nil), raw...)
	return nil
}

func (a *API) listObjectTypes(w http.ResponseWriter, r *http.Request) {
	gameID := r.PathValue("gameID")
	if _, err := a.loadGame(r, gameID, false); err != nil {
		notFoundOrError(w, err, "Game")
		return
	}
	rows, err := a.db.QueryContext(r.Context(), `SELECT id,game_id,name,description,icon,image,sort_order,archived,primary_display_field_id FROM object_types WHERE game_id=? AND (archived=0 OR ?=1) ORDER BY sort_order,name`, gameID, boolInt(r.URL.Query().Get("archived") == "true"))
	if err != nil {
		writeError(w, http.StatusInternalServerError, "database_error", "Could not load object types.")
		return
	}
	defer rows.Close()
	items := make([]objectType, 0)
	for rows.Next() {
		var item objectType
		if err := rows.Scan(&item.ID, &item.GameID, &item.Name, &item.Description, &item.Icon, &item.Image, &item.SortOrder, &item.Archived, &item.PrimaryDisplayFieldID); err != nil {
			writeError(w, http.StatusInternalServerError, "database_error", "Could not load object types.")
			return
		}
		items = append(items, item)
	}
	if err := rows.Err(); err != nil {
		writeError(w, http.StatusInternalServerError, "database_error", "Could not load object types.")
		return
	}
	if err := rows.Close(); err != nil {
		writeError(w, http.StatusInternalServerError, "database_error", "Could not load object types.")
		return
	}
	for i := range items {
		items[i].Fields, err = a.loadFields(r.Context(), items[i].ID, r.URL.Query().Get("archived") == "true")
		if err != nil {
			writeError(w, http.StatusInternalServerError, "database_error", "Could not load field definitions.")
			return
		}
	}
	writeJSON(w, http.StatusOK, map[string]any{"items": items})
}

func (a *API) createObjectType(w http.ResponseWriter, r *http.Request) {
	gameID := r.PathValue("gameID")
	if _, err := a.loadGame(r, gameID, false); err != nil {
		notFoundOrError(w, err, "Game")
		return
	}
	var input struct {
		Name        string `json:"name"`
		Description string `json:"description"`
		Icon        string `json:"icon"`
		Image       string `json:"image"`
	}
	if !decodeJSON(w, r, &input) {
		return
	}
	input.Name = strings.TrimSpace(input.Name)
	if input.Name == "" || len(input.Name) > 80 {
		writeError(w, http.StatusBadRequest, "invalid_name", "Object type name must be between 1 and 80 characters.")
		return
	}
	if input.Icon == "" {
		input.Icon = "◈"
	}
	input.Image = canonicalImageURL(r, input.Image)
	if !a.validateImage(r, input.Image) {
		writeError(w, 400, "invalid_image", "Choose an image URL or an upload you own.")
		return
	}
	input.Icon = canonicalImageURL(r, input.Icon)
	if !a.validIcon(r, input.Icon) {
		writeError(w, 400, "invalid_icon", "Choose a symbol or an image you own.")
		return
	}
	var order int
	if err := a.db.QueryRowContext(r.Context(), `SELECT COALESCE(MAX(sort_order)+1,0) FROM object_types WHERE game_id=?`, gameID).Scan(&order); err != nil {
		writeError(w, http.StatusInternalServerError, "database_error", "Could not create object type.")
		return
	}
	id, err := randomID()
	if err != nil {
		writeError(w, http.StatusInternalServerError, "create_failed", "Could not create object type.")
		return
	}
	now := time.Now().UTC().Format(time.RFC3339)
	_, err = a.db.ExecContext(r.Context(), `INSERT INTO object_types(id,game_id,name,description,icon,image,sort_order,created_at,updated_at) VALUES(?,?,?,?,?,?,?,?,?)`, id, gameID, input.Name, input.Description, input.Icon, input.Image, order, now, now)
	if err != nil {
		writeError(w, http.StatusInternalServerError, "create_failed", "Could not create object type.")
		return
	}
	writeJSON(w, http.StatusCreated, objectType{ID: id, GameID: gameID, Name: input.Name, Description: input.Description, Icon: input.Icon, Image: input.Image, SortOrder: order, Fields: []fieldDefinition{}})
}

func (a *API) getObjectType(w http.ResponseWriter, r *http.Request) {
	item, err := a.loadObjectType(r, r.PathValue("typeID"))
	if err != nil {
		notFoundOrError(w, err, "Object type")
		return
	}
	item.Fields, err = a.loadFields(r.Context(), item.ID, r.URL.Query().Get("archived") == "true")
	if err != nil {
		writeError(w, http.StatusInternalServerError, "database_error", "Could not load fields.")
		return
	}
	writeJSON(w, http.StatusOK, item)
}

func (a *API) updateObjectType(w http.ResponseWriter, r *http.Request) {
	item, err := a.loadObjectType(r, r.PathValue("typeID"))
	if err != nil {
		notFoundOrError(w, err, "Object type")
		return
	}
	var input struct {
		PrimaryDisplayFieldID *string `json:"primary_display_field_id"`
		Name                  *string `json:"name"`
		Description           *string `json:"description"`
		Icon                  *string `json:"icon"`
		Image                 *string `json:"image"`
	}
	if !decodeJSON(w, r, &input) {
		return
	}
	if input.Name != nil {
		name := strings.TrimSpace(*input.Name)
		if name == "" || len(name) > 80 {
			writeError(w, http.StatusBadRequest, "invalid_name", "Object type name must be between 1 and 80 characters.")
			return
		}
		item.Name = name
	}
	if input.Description != nil {
		item.Description = *input.Description
	}
	if input.Icon != nil {
		item.Icon = *input.Icon
	}
	if input.Image != nil {
		item.Image = *input.Image
	}
	if input.PrimaryDisplayFieldID != nil {
		if *input.PrimaryDisplayFieldID != "" && *input.PrimaryDisplayFieldID != item.PrimaryDisplayFieldID {
			var valid int
			if err := a.db.QueryRowContext(r.Context(), `SELECT COUNT(*) FROM field_definitions WHERE id=? AND object_type_id=? AND archived=0`, *input.PrimaryDisplayFieldID, item.ID).Scan(&valid); err != nil || valid != 1 {
				writeError(w, 400, "invalid_primary_field", "Choose an active field belonging to this object type.")
				return
			}
		}
		item.PrimaryDisplayFieldID = *input.PrimaryDisplayFieldID
	}
	item.Image = canonicalImageURL(r, item.Image)
	if !a.validateImage(r, item.Image) {
		writeError(w, 400, "invalid_image", "Choose an image URL or an upload you own.")
		return
	}
	item.Icon = canonicalImageURL(r, item.Icon)
	if !a.validIcon(r, item.Icon) {
		writeError(w, 400, "invalid_icon", "Choose a symbol or an image you own.")
		return
	}
	_, err = a.db.ExecContext(r.Context(), `UPDATE object_types SET name=?,description=?,icon=?,image=?,primary_display_field_id=?,updated_at=? WHERE id=?`, item.Name, item.Description, item.Icon, item.Image, item.PrimaryDisplayFieldID, time.Now().UTC().Format(time.RFC3339), item.ID)
	if err != nil {
		writeError(w, http.StatusInternalServerError, "update_failed", "Could not update object type.")
		return
	}
	writeJSON(w, http.StatusOK, item)
}

func (a *API) createField(w http.ResponseWriter, r *http.Request) {
	typeID := r.PathValue("typeID")
	typ, err := a.loadObjectType(r, typeID)
	if err != nil {
		notFoundOrError(w, err, "Object type")
		return
	}
	var input struct {
		Name         string          `json:"name"`
		FieldType    string          `json:"field_type"`
		Icon         string          `json:"icon"`
		Description  string          `json:"description"`
		DefaultValue json.RawMessage `json:"default_value"`
		Options      json.RawMessage `json:"options"`
		SortOrder    *int            `json:"sort_order"`
	}
	if !decodeJSON(w, r, &input) {
		return
	}
	input.Name = strings.TrimSpace(input.Name)
	if input.Name == "" || len(input.Name) > 80 {
		writeError(w, http.StatusBadRequest, "invalid_name", "Field name must be between 1 and 80 characters.")
		return
	}
	if !supportedFieldTypes[input.FieldType] {
		writeError(w, http.StatusBadRequest, "invalid_field_type", "Choose one of the supported field types.")
		return
	}
	if input.Icon == "" {
		input.Icon = "✦"
	}
	if len(input.Options) == 0 {
		input.Options = json.RawMessage(`{}`)
	}
	if !json.Valid(input.Options) {
		writeError(w, http.StatusBadRequest, "invalid_options", "Field options must be valid JSON.")
		return
	}
	if err := validateFieldOptions(input.FieldType, input.Options); err != nil {
		writeError(w, http.StatusBadRequest, "invalid_options", err.Error())
		return
	}
	if err := a.validateFieldTypeOptions(r.Context(), typ.GameID, input.FieldType, input.Options); err != nil {
		writeError(w, http.StatusBadRequest, "invalid_options", err.Error())
		return
	}
	if len(input.DefaultValue) > 0 && string(input.DefaultValue) != "null" {
		if err := validateDefaultValue(input.FieldType, input.Options, input.DefaultValue); err != nil {
			writeError(w, http.StatusBadRequest, "invalid_default", err.Error())
			return
		}
	}
	input.Icon = canonicalImageURL(r, input.Icon)
	if !a.validIcon(r, input.Icon) {
		writeError(w, 400, "invalid_icon", "Choose a symbol or an image you own.")
		return
	}
	var order int
	if err := a.db.QueryRowContext(r.Context(), `SELECT COALESCE(MAX(sort_order)+1,0) FROM field_definitions WHERE object_type_id=?`, typeID).Scan(&order); err != nil {
		writeError(w, http.StatusInternalServerError, "database_error", "Could not create field.")
		return
	}
	if input.SortOrder != nil {
		order = *input.SortOrder
	}
	id, err := randomID()
	if err != nil {
		writeError(w, http.StatusInternalServerError, "create_failed", "Could not create field.")
		return
	}
	now := time.Now().UTC().Format(time.RFC3339)
	_, err = a.db.ExecContext(r.Context(), `INSERT INTO field_definitions(id,object_type_id,name,field_type,icon,description,default_json,options_json,sort_order,created_at,updated_at) VALUES(?,?,?,?,?,?,?,?,?,?,?)`, id, typeID, input.Name, input.FieldType, input.Icon, input.Description, nullableJSON(input.DefaultValue), string(input.Options), order, now, now)
	if err != nil {
		writeError(w, http.StatusInternalServerError, "create_failed", "Could not create field.")
		return
	}
	if input.FieldType == "formula" {
		if err := a.validateFormulaSchema(r.Context(), typ.ID, id, input.Options); err != nil {
			_, _ = a.db.ExecContext(r.Context(), `DELETE FROM field_definitions WHERE id=?`, id)
			writeError(w, http.StatusBadRequest, "invalid_formula", err.Error())
			return
		}
	}
	writeJSON(w, http.StatusCreated, fieldDefinition{ID: id, ObjectTypeID: typeID, Name: input.Name, FieldType: input.FieldType, Icon: input.Icon, Description: input.Description, DefaultValue: input.DefaultValue, Options: input.Options, SortOrder: order})
}

func (a *API) updateField(w http.ResponseWriter, r *http.Request) {
	item, gameID, err := a.loadField(r, r.PathValue("fieldID"))
	if err != nil {
		notFoundOrError(w, err, "Field")
		return
	}
	var input struct {
		Name                         *string            `json:"name"`
		FieldType                    *string            `json:"field_type"`
		Icon                         *string            `json:"icon"`
		Description                  *string            `json:"description"`
		DefaultValue                 optionalRawMessage `json:"default_value"`
		Options                      optionalRawMessage `json:"options"`
		SortOrder                    *int               `json:"sort_order"`
		ChoiceReplacements           map[string]string  `json:"choice_replacements"`
		RemoveIncompatibleReferences bool               `json:"remove_incompatible_references"`
	}
	if !decodeJSON(w, r, &input) {
		return
	}
	if input.FieldType != nil {
		if *input.FieldType != item.FieldType {
			writeError(w, http.StatusConflict, "field_type_immutable", "A field's type cannot be changed after creation.")
			return
		}
	}
	if input.Name != nil {
		item.Name = strings.TrimSpace(*input.Name)
		if item.Name == "" || len(item.Name) > 80 {
			writeError(w, http.StatusBadRequest, "invalid_name", "Field name must be between 1 and 80 characters.")
			return
		}
	}
	if input.Icon != nil {
		item.Icon = *input.Icon
	}
	if input.Description != nil {
		item.Description = *input.Description
	}
	if input.SortOrder != nil {
		item.SortOrder = *input.SortOrder
	}
	if input.Options.Present {
		opts := input.Options.Value
		if !json.Valid(opts) {
			writeError(w, http.StatusBadRequest, "invalid_options", "Field options must be valid JSON.")
			return
		}
		if err := validateFieldOptions(item.FieldType, opts); err != nil {
			writeError(w, http.StatusBadRequest, "invalid_options", err.Error())
			return
		}
		if err := a.validateFieldTypeOptions(r.Context(), gameID, item.FieldType, opts); err != nil {
			writeError(w, http.StatusBadRequest, "invalid_options", err.Error())
			return
		}
		if item.FieldType == "formula" {
			if err := a.validateFormulaSchema(r.Context(), item.ObjectTypeID, item.ID, opts); err != nil {
				writeError(w, http.StatusBadRequest, "invalid_formula", err.Error())
				return
			}
		}
		item.Options = opts
	}
	if input.DefaultValue.Present {
		defaultValue := input.DefaultValue.Value
		if string(defaultValue) == "null" {
			item.DefaultValue = nil
		} else {
			if !json.Valid(defaultValue) {
				writeError(w, http.StatusBadRequest, "invalid_default", "Default value must be valid JSON.")
				return
			}
			item.DefaultValue = defaultValue
		}
	}
	item.Icon = canonicalImageURL(r, item.Icon)
	if !a.validIcon(r, item.Icon) {
		writeError(w, 400, "invalid_icon", "Choose a symbol or an image you own.")
		return
	}
	tx, err := a.db.BeginTx(r.Context(), nil)
	if err != nil {
		writeError(w, 500, "update_failed", "Could not update field.")
		return
	}
	defer tx.Rollback()
	if err := migrateFieldOptions(r.Context(), tx, &item, input.ChoiceReplacements, input.RemoveIncompatibleReferences); err != nil {
		if validation, ok := err.(validationError); ok {
			writeError(w, http.StatusConflict, validation.code, validation.message)
		} else {
			writeError(w, 500, "update_failed", "Could not validate existing field data.")
		}
		return
	}
	_, err = tx.ExecContext(r.Context(), `UPDATE field_definitions SET name=?,icon=?,description=?,default_json=?,options_json=?,sort_order=?,updated_at=? WHERE id=?`, item.Name, item.Icon, item.Description, nullableJSON(item.DefaultValue), string(item.Options), item.SortOrder, time.Now().UTC().Format(time.RFC3339), item.ID)
	if err != nil {
		writeError(w, http.StatusInternalServerError, "update_failed", "Could not update field definition.")
		return
	}
	if err := tx.Commit(); err != nil {
		writeError(w, 500, "update_failed", "Could not update field.")
		return
	}
	writeJSON(w, http.StatusOK, item)
}

func (a *API) archiveField(w http.ResponseWriter, r *http.Request) { a.setFieldArchived(w, r, true) }
func (a *API) restoreField(w http.ResponseWriter, r *http.Request) { a.setFieldArchived(w, r, false) }
func (a *API) setFieldArchived(w http.ResponseWriter, r *http.Request, archived bool) {
	item, _, err := a.loadField(r, r.PathValue("fieldID"))
	if err != nil {
		notFoundOrError(w, err, "Field")
		return
	}
	tx, err := a.db.BeginTx(r.Context(), nil)
	if err != nil {
		writeError(w, http.StatusInternalServerError, "update_failed", "Could not update field.")
		return
	}
	defer tx.Rollback()
	if archived {
		dependencies, err := a.fieldDependencies(r.Context(), tx, item.ID)
		if err != nil {
			writeError(w, http.StatusInternalServerError, "database_error", "Could not check field dependencies.")
			return
		}
		if len(dependencies) > 0 {
			message := fmt.Sprintf("Cannot archive %q. This field is currently used by:\n\n- %s\n\nUpdate these dependencies before archiving the field.", item.Name, strings.Join(dependencies, "\n- "))
			writeJSON(w, http.StatusConflict, map[string]any{"error": map[string]string{"code": "field_in_use", "message": message}, "dependencies": dependencies})
			return
		}
	}
	_, err = tx.ExecContext(r.Context(), `UPDATE field_definitions SET archived=?,updated_at=? WHERE id=?`, boolInt(archived), time.Now().UTC().Format(time.RFC3339), item.ID)
	if err != nil {
		writeError(w, http.StatusInternalServerError, "update_failed", "Could not update field.")
		return
	}
	if err := tx.Commit(); err != nil {
		writeError(w, http.StatusInternalServerError, "update_failed", "Could not update field.")
		return
	}
	w.WriteHeader(http.StatusNoContent)
}

func (a *API) loadObjectType(r *http.Request, id string) (objectType, error) {
	var item objectType
	err := a.db.QueryRowContext(r.Context(), `SELECT t.id,t.game_id,t.name,t.description,t.icon,t.image,t.sort_order,t.archived,t.primary_display_field_id FROM object_types t JOIN games g ON g.id=t.game_id WHERE t.id=? AND g.user_id=? AND g.archived=0`, id, a.currentUser(r).ID).Scan(&item.ID, &item.GameID, &item.Name, &item.Description, &item.Icon, &item.Image, &item.SortOrder, &item.Archived, &item.PrimaryDisplayFieldID)
	return item, err
}

func (a *API) loadFields(ctx context.Context, typeID string, includeArchived bool) ([]fieldDefinition, error) {
	cache, _ := ctx.Value(readCacheKey{}).(*readCache)
	key := fieldCacheKey{id: typeID, archived: includeArchived}
	if cache != nil {
		if fields, ok := cache.fields[key]; ok {
			return fields, nil
		}
	}
	fields, err := loadFieldsFrom(ctx, a.db, typeID, includeArchived)
	if err == nil && cache != nil {
		cache.fields[key] = fields
	}
	return fields, err
}

func loadFieldsFrom(ctx context.Context, db dbExecutor, typeID string, includeArchived bool) ([]fieldDefinition, error) {
	query := `SELECT id,object_type_id,name,field_type,icon,description,default_json,options_json,sort_order,archived FROM field_definitions WHERE object_type_id=?`
	if !includeArchived {
		query += ` AND archived=0`
	}
	query += ` ORDER BY sort_order,name`
	rows, err := db.QueryContext(ctx, query, typeID)
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	items := make([]fieldDefinition, 0)
	for rows.Next() {
		var item fieldDefinition
		var defaultJSON, optionsJSON []byte
		var archived int
		if err := rows.Scan(&item.ID, &item.ObjectTypeID, &item.Name, &item.FieldType, &item.Icon, &item.Description, &defaultJSON, &optionsJSON, &item.SortOrder, &archived); err != nil {
			return nil, err
		}
		if len(defaultJSON) > 0 {
			item.DefaultValue = append(json.RawMessage(nil), defaultJSON...)
		}
		if len(optionsJSON) > 0 {
			item.Options = append(json.RawMessage(nil), optionsJSON...)
		} else {
			item.Options = json.RawMessage(`{}`)
		}
		item.Archived = archived != 0
		items = append(items, item)
	}
	return items, rows.Err()
}

func (a *API) loadField(r *http.Request, id string) (fieldDefinition, string, error) {
	var item fieldDefinition
	var gameID string
	var archived int
	var defaultJSON, optionsJSON []byte
	err := a.db.QueryRowContext(r.Context(), `SELECT f.id,f.object_type_id,f.name,f.field_type,f.icon,f.description,f.default_json,f.options_json,f.sort_order,f.archived,t.game_id FROM field_definitions f JOIN object_types t ON t.id=f.object_type_id JOIN games g ON g.id=t.game_id WHERE f.id=? AND g.user_id=? AND g.archived=0`, id, a.currentUser(r).ID).Scan(&item.ID, &item.ObjectTypeID, &item.Name, &item.FieldType, &item.Icon, &item.Description, &defaultJSON, &optionsJSON, &item.SortOrder, &archived, &gameID)
	if len(defaultJSON) > 0 {
		item.DefaultValue = append(json.RawMessage(nil), defaultJSON...)
	}
	item.Options = append(json.RawMessage(nil), optionsJSON...)
	item.Archived = archived != 0
	return item, gameID, err
}

func validateFieldOptions(fieldType string, raw json.RawMessage) error {
	var options map[string]any
	if len(raw) > 0 && json.Unmarshal(raw, &options) != nil {
		return errText("Field options must be a JSON object.")
	}
	if options == nil {
		options = map[string]any{}
	}
	switch fieldType {
	case "select":
		choices, ok := options["choices"].([]any)
		if !ok || len(choices) == 0 || len(choices) > 200 {
			return errText("Select fields need at least one choice.")
		}
		seen := map[string]bool{}
		for _, choice := range choices {
			text, ok := choice.(string)
			if !ok || strings.TrimSpace(text) == "" || len(text) > 100 {
				return errText("Select choices must be nonempty text values up to 100 characters.")
			}
			if seen[text] {
				return errText("Select choices must be unique.")
			}
			seen[text] = true
		}
	case "reference", "references":
		if allowed, ok := options["allowed_type_id"]; ok {
			if _, ok := allowed.(string); !ok {
				return errText("allowed_type_id must be a type ID.")
			}
		}
	case "formula":
		expr, ok := options["expression"].(string)
		if !ok || strings.TrimSpace(expr) == "" || len(expr) > 4096 {
			return errText("Formula fields need an expression.")
		}
	}
	return nil
}

func validateDefaultValue(fieldType string, options, raw json.RawMessage) error {
	if fieldType == "formula" || fieldType == "reference" || fieldType == "references" {
		return errText("Formula and object-reference fields cannot have a default value.")
	}
	field := fieldDefinition{FieldType: fieldType, Options: options}
	_, _, _, err := validateFieldValue(field, raw)
	return err
}

func (a *API) validateFieldTypeOptions(ctx context.Context, gameID, fieldType string, raw json.RawMessage) error {
	if fieldType != "reference" && fieldType != "references" {
		return nil
	}
	var options struct {
		AllowedTypeID string `json:"allowed_type_id"`
	}
	if err := json.Unmarshal(raw, &options); err != nil {
		return errText("Reference options must be valid JSON.")
	}
	if options.AllowedTypeID == "" {
		return nil
	}
	var count int
	if err := a.db.QueryRowContext(ctx, `SELECT COUNT(*) FROM object_types WHERE id=? AND game_id=?`, options.AllowedTypeID, gameID).Scan(&count); err != nil || count == 0 {
		return errText("Allowed target type must belong to the same game.")
	}
	return nil
}

func (a *API) validateFormulaSchema(ctx context.Context, typeID, fieldID string, options json.RawMessage) error {
	var config struct {
		Expression  string                   `json:"expression"`
		Aggregation *relationshipAggregation `json:"aggregation"`
	}
	if err := json.Unmarshal(options, &config); err != nil {
		return errText("Formula options must contain an expression.")
	}
	if err := formula.Validate(config.Expression); err != nil {
		return err
	}
	if config.Aggregation != nil {
		if err := a.validateAggregation(ctx, typeID, *config.Aggregation); err != nil {
			return err
		}
	}
	refs := formula.References(config.Expression)
	if len(refs) == 0 {
		return errText("Formula must reference at least one numeric field.")
	}
	for _, ref := range refs {
		if ref == "related" && config.Aggregation != nil {
			continue
		}
		if ref == fieldID {
			return errText("A formula cannot reference itself.")
		}
		var typ string
		if err := a.db.QueryRowContext(ctx, `SELECT field_type FROM field_definitions WHERE id=? AND object_type_id=? AND archived=0`, ref, typeID).Scan(&typ); err != nil {
			return errText("Every formula reference must be a field on this object type.")
		}
		if typ != "integer" && typ != "decimal" && typ != "percentage" && typ != "progress" && typ != "formula" {
			return errText("Formulas can reference numeric or formula fields only.")
		}
	}
	// Traverse formulas recursively, rejecting indirect cycles before saving an edit.
	visited := map[string]bool{}
	var visit func(string) error
	visit = func(id string) error {
		if id == "related" {
			return nil
		}
		if len(visited) > 128 {
			return errText("Formula dependencies exceed 128 fields.")
		}
		if id == fieldID {
			return errText("This formula would create a circular dependency.")
		}
		if visited[id] {
			return nil
		}
		visited[id] = true
		var typ string
		var raw []byte
		if err := a.db.QueryRowContext(ctx, `SELECT field_type,options_json FROM field_definitions WHERE id=? AND object_type_id=?`, id, typeID).Scan(&typ, &raw); err != nil {
			return nil
		}
		if typ != "formula" {
			return nil
		}
		var cfg struct {
			Expression string `json:"expression"`
		}
		_ = json.Unmarshal(raw, &cfg)
		for _, dep := range formula.References(cfg.Expression) {
			if err := visit(dep); err != nil {
				return err
			}
		}
		return nil
	}
	for _, ref := range refs {
		if err := visit(ref); err != nil {
			return err
		}
	}
	return nil
}

func (a *API) fieldDependencies(ctx context.Context, db dbExecutor, fieldID string) ([]string, error) {
	deps := make([]string, 0)
	var gameID string
	if err := db.QueryRowContext(ctx, `SELECT t.game_id FROM field_definitions f JOIN object_types t ON t.id=f.object_type_id WHERE f.id=?`, fieldID).Scan(&gameID); err != nil {
		return nil, err
	}
	var goalCount int
	if err := db.QueryRowContext(ctx, `SELECT COUNT(*) FROM goals WHERE game_id=? AND (field_id=? OR EXISTS (SELECT 1 FROM json_each(goals.target_json, '$.conditions') c WHERE json_extract(c.value, '$.field_id')=?))`, gameID, fieldID, fieldID).Scan(&goalCount); err != nil {
		return nil, err
	}
	if goalCount > 0 {
		deps = append(deps, "goal conditions")
	}
	var relationshipCount int
	if err := db.QueryRowContext(ctx, `SELECT COUNT(*) FROM object_relationships WHERE field_id=?`, fieldID).Scan(&relationshipCount); err != nil {
		return nil, err
	}
	if relationshipCount > 0 {
		deps = append(deps, "object relationships")
	}
	rows, err := db.QueryContext(ctx, `SELECT f.id,f.name,f.options_json FROM field_definitions f JOIN object_types t ON t.id=f.object_type_id WHERE f.field_type='formula' AND t.game_id=?`, gameID)
	if err != nil {
		return nil, err
	}
	type formulaDependency struct {
		name string
		refs []string
	}
	formulas := make(map[string]formulaDependency)
	for rows.Next() {
		var id, name string
		var raw []byte
		if err := rows.Scan(&id, &name, &raw); err != nil {
			rows.Close()
			return nil, err
		}
		var cfg struct {
			Expression  string                   `json:"expression"`
			Aggregation *relationshipAggregation `json:"aggregation"`
		}
		_ = json.Unmarshal(raw, &cfg)
		refs := formula.References(cfg.Expression)
		if cfg.Aggregation != nil {
			refs = append(refs, cfg.Aggregation.RelationshipFieldID, cfg.Aggregation.FieldID)
		}
		formulas[id] = formulaDependency{name: name, refs: refs}
	}
	if err := rows.Err(); err != nil {
		rows.Close()
		return nil, err
	}
	if err := rows.Close(); err != nil {
		return nil, err
	}
	for id, candidate := range formulas {
		seen := make(map[string]bool)
		var dependsOn func(string) bool
		dependsOn = func(current string) bool {
			if current == fieldID {
				return true
			}
			if seen[current] {
				return false
			}
			seen[current] = true
			for _, ref := range formulas[current].refs {
				if ref == fieldID || dependsOn(ref) {
					return true
				}
			}
			return false
		}
		if dependsOn(id) {
			deps = append(deps, "formula: "+candidate.name)
		}
	}
	sectionRows, err := db.QueryContext(ctx, `SELECT name,display_config,archived FROM sections WHERE game_id=? ORDER BY sort_order,name,id`, gameID)
	if err != nil {
		return nil, err
	}
	for sectionRows.Next() {
		var name string
		var raw []byte
		var archived int
		if err := sectionRows.Scan(&name, &raw, &archived); err != nil {
			sectionRows.Close()
			return nil, err
		}
		sectionLabel := name
		if archived != 0 {
			sectionLabel = "archived section: " + name
		}
		var display map[string]json.RawMessage
		if len(raw) > 0 {
			if err := json.Unmarshal(raw, &display); err != nil {
				sectionRows.Close()
				return nil, fmt.Errorf("invalid saved section display configuration: %w", err)
			}
		}
		viewDependencies, err := savedViewFieldDependencies(display, fieldID)
		if err != nil {
			sectionRows.Close()
			return nil, fmt.Errorf("invalid saved object view: %w", err)
		}
		for _, dependency := range viewDependencies {
			deps = append(deps, sectionLabel+": "+dependency)
		}
		var config struct {
			VisibleFields      []string `json:"visible_field_ids"`
			ChecklistFieldID   string   `json:"checklist_field_id"`
			ProgressFieldID    string   `json:"progress_field_id"`
			CurrentFields      []string `json:"progress_current_field_ids"`
			MaximumFields      []string `json:"progress_max_field_ids"`
			RelationshipField  string   `json:"progress_relationship_field_id"`
			ChildProgressField string   `json:"progress_child_field_id"`
		}
		if len(raw) > 0 {
			if err := json.Unmarshal(raw, &config); err != nil {
				sectionRows.Close()
				return nil, fmt.Errorf("invalid saved section display configuration: %w", err)
			}
		}
		for _, id := range config.VisibleFields {
			if id == fieldID {
				deps = append(deps, sectionLabel+": visible fields")
				break
			}
		}
		for _, id := range append(append([]string{}, config.CurrentFields...), append(config.MaximumFields, config.RelationshipField, config.ChildProgressField)...) {
			if id == fieldID {
				deps = append(deps, sectionLabel+": calculated progress")
				break
			}
		}
		if config.ChecklistFieldID == fieldID {
			deps = append(deps, sectionLabel+": checklist view")
		}
		if config.ProgressFieldID == fieldID {
			deps = append(deps, sectionLabel+": progress view")
		}
	}
	if err := sectionRows.Err(); err != nil {
		sectionRows.Close()
		return nil, err
	}
	if err := sectionRows.Close(); err != nil {
		return nil, err
	}
	sort.Strings(deps)
	return deps, nil
}

// savedViewFieldDependencies inspects parsed JSON properties whose names denote
// stable field references. It never searches serialized JSON text or arbitrary
// values, which could produce false dependencies from titles or filter targets.
func savedViewFieldDependencies(config map[string]json.RawMessage, fieldID string) ([]string, error) {
	viewRaw := config["object_view"]
	if len(viewRaw) == 0 {
		return nil, nil
	}
	var view map[string]any
	if err := json.Unmarshal(viewRaw, &view); err != nil || view == nil {
		return nil, fmt.Errorf("object_view must be a JSON object")
	}
	var validated objectView
	if err := json.Unmarshal(viewRaw, &validated); err != nil {
		return nil, fmt.Errorf("object_view has an invalid structure")
	}
	used := map[string]bool{}
	var walk func(any, string)
	walk = func(value any, path string) {
		switch item := value.(type) {
		case map[string]any:
			for key, child := range item {
				childPath := key
				if path != "" {
					childPath = path + "." + key
				}
				if key == "sort" {
					if sortID, ok := child.(string); ok && sortID == fieldID {
						used["sorting"] = true
					}
				}
				if (key == "field_id" || strings.HasSuffix(key, "_field_id")) && child == fieldID {
					if strings.Contains(path, "conditions") {
						used["filter"] = true
					} else {
						used["object view ("+childPath+")"] = true
					}
				}
				if key == "field_ids" || strings.HasSuffix(key, "_field_ids") {
					if ids, ok := child.([]any); ok {
						for _, rawID := range ids {
							if rawID == fieldID {
								used["object view ("+childPath+")"] = true
							}
						}
					}
				}
				walk(child, childPath)
			}
		case []any:
			for _, child := range item {
				walk(child, path)
			}
		}
	}
	walk(view, "")
	result := make([]string, 0, len(used))
	for _, name := range []string{"filter", "sorting"} {
		if used[name] {
			result = append(result, "saved object view: "+name)
			delete(used, name)
		}
	}
	for name := range used {
		result = append(result, name)
	}
	sort.Strings(result)
	return result, nil
}

type textError string

func (e textError) Error() string { return string(e) }
func errText(s string) error      { return textError(s) }
func nullableJSON(raw json.RawMessage) any {
	if len(raw) == 0 {
		return nil
	}
	return string(raw)
}
