package httpapi

import (
	"context"
	"encoding/json"
	"errors"
	"net/http"
	"sort"
	"strings"
	"time"
)

const maxObjectPresets = 100
const maxPresetBytes = 1 << 20

type objectPreset struct {
	ID              string                     `json:"id"`
	ObjectID        string                     `json:"object_id"`
	Name            string                     `json:"name"`
	Description     string                     `json:"description"`
	SortOrder       int                        `json:"sort_order"`
	Values          map[string]json.RawMessage `json:"values"`
	EffectiveValues map[string]json.RawMessage `json:"effective_values,omitempty"`
	FormulaErrors   map[string]string          `json:"formula_errors,omitempty"`
	CreatedAt       string                     `json:"created_at"`
	UpdatedAt       string                     `json:"updated_at"`
}

func (a *API) listObjectPresets(w http.ResponseWriter, r *http.Request) {
	item, err := a.loadObjectAny(r, r.PathValue("objectID"))
	if err != nil {
		notFoundOrError(w, err, "Object")
		return
	}
	fields, err := loadFieldsFrom(r.Context(), a.db, item.ObjectTypeID, false)
	if err != nil {
		writeError(w, http.StatusInternalServerError, "database_error", "Could not load object fields.")
		return
	}
	canonical, err := a.objectValues(r.Context(), a.db, item, fields)
	if err != nil {
		writeError(w, http.StatusInternalServerError, "database_error", "Could not read object values.")
		return
	}
	rows, err := a.db.QueryContext(r.Context(), `SELECT id,object_id,name,description,sort_order,values_json,created_at,updated_at FROM object_presets WHERE object_id=? AND game_id=? ORDER BY sort_order,created_at,id`, item.ID, item.GameID)
	if err != nil {
		writeError(w, http.StatusInternalServerError, "database_error", "Could not load object presets.")
		return
	}
	presets := []objectPreset{}
	for rows.Next() {
		preset, err := scanObjectPreset(rows)
		if err != nil {
			rows.Close()
			writeError(w, http.StatusInternalServerError, "database_error", "Could not load object presets.")
			return
		}
		presets = append(presets, preset)
	}
	if err := rows.Err(); err != nil {
		rows.Close()
		writeError(w, http.StatusInternalServerError, "database_error", "Could not load object presets.")
		return
	}
	if err := rows.Close(); err != nil {
		writeError(w, http.StatusInternalServerError, "database_error", "Could not load object presets.")
		return
	}
	for index := range presets {
		presets[index].EffectiveValues = mergePresetValues(canonical, presets[index].Values)
		if err := a.calculateFormulasWithOverrides(r.Context(), a.db, item, fields, presets[index].EffectiveValues, presets[index].Values); err != nil {
			writeError(w, http.StatusInternalServerError, "preset_failed", "Could not calculate preset formulas.")
			return
		}
		presets[index].FormulaErrors = formulaErrors(fields, presets[index].EffectiveValues)
	}
	writeJSON(w, http.StatusOK, map[string]any{"items": presets, "fields": fields, "canonical_values": canonical, "object_archived": item.Archived})
}

func (a *API) createObjectPreset(w http.ResponseWriter, r *http.Request) {
	item, err := a.loadObjectAny(r, r.PathValue("objectID"))
	if err != nil {
		notFoundOrError(w, err, "Object")
		return
	}
	var input struct {
		Name        string                     `json:"name"`
		Description string                     `json:"description"`
		Values      map[string]json.RawMessage `json:"values"`
	}
	if !decodeJSON(w, r, &input) {
		return
	}
	input.Name = strings.TrimSpace(input.Name)
	if err := validatePresetMetadata(input.Name, input.Description); err != nil {
		writeError(w, http.StatusBadRequest, "invalid_preset", err.Error())
		return
	}
	tx, err := a.db.BeginTx(r.Context(), nil)
	if err != nil {
		writeError(w, http.StatusInternalServerError, "preset_failed", "Could not create this preset.")
		return
	}
	defer tx.Rollback()
	fields, err := loadFieldsFrom(r.Context(), tx, item.ObjectTypeID, false)
	if err != nil {
		writeError(w, http.StatusInternalServerError, "database_error", "Could not load object fields.")
		return
	}
	values, err := validatePresetValues(r.Context(), tx, item, fields, input.Values)
	if err != nil {
		writeValidationError(w, err)
		return
	}
	var count, order int
	if err := tx.QueryRowContext(r.Context(), `SELECT COUNT(*),COALESCE(MAX(sort_order),-1)+1 FROM object_presets WHERE object_id=?`, item.ID).Scan(&count, &order); err != nil {
		writeError(w, http.StatusInternalServerError, "database_error", "Could not validate the preset limit.")
		return
	}
	if count >= maxObjectPresets {
		writeError(w, http.StatusConflict, "preset_limit", "An object can have at most 100 presets.")
		return
	}
	encoded, err := json.Marshal(values)
	if err != nil || len(encoded) > maxPresetBytes {
		writeError(w, http.StatusRequestEntityTooLarge, "preset_too_large", "Preset overrides exceed the 1 MB limit.")
		return
	}
	id, err := randomID()
	if err != nil {
		writeError(w, http.StatusInternalServerError, "preset_failed", "Could not create this preset.")
		return
	}
	now := time.Now().UTC().Format(time.RFC3339Nano)
	if _, err := tx.ExecContext(r.Context(), `INSERT INTO object_presets(id,object_id,game_id,name,description,sort_order,values_json,created_at,updated_at) VALUES(?,?,?,?,?,?,?,?,?)`, id, item.ID, item.GameID, input.Name, input.Description, order, string(encoded), now, now); err != nil {
		writeError(w, http.StatusInternalServerError, "preset_failed", "Could not create this preset.")
		return
	}
	changes := presetHistoryChanges(input.Name, fields, nil, values)
	changes = append(changes, objectHistoryChange{FieldName: "Preset created: " + input.Name, FieldType: "preset", After: marshalRaw(input.Name)})
	if err := a.writeObjectHistory(r.Context(), tx, a.currentUser(r).ID, item, "updated", "api", historyTime(time.Now()), changes); err != nil {
		writeError(w, http.StatusInternalServerError, "preset_failed", "Could not record preset history.")
		return
	}
	if err := tx.Commit(); err != nil {
		writeError(w, http.StatusInternalServerError, "preset_failed", "Could not create this preset.")
		return
	}
	writeJSON(w, http.StatusCreated, objectPreset{ID: id, ObjectID: item.ID, Name: input.Name, Description: input.Description, SortOrder: order, Values: values, CreatedAt: now, UpdatedAt: now})
}

func (a *API) updateObjectPreset(w http.ResponseWriter, r *http.Request) {
	item, preset, err := a.loadObjectPreset(r, r.PathValue("presetID"))
	if err != nil {
		notFoundOrError(w, err, "Preset")
		return
	}
	var input struct {
		Name        *string                    `json:"name"`
		Description *string                    `json:"description"`
		Values      map[string]json.RawMessage `json:"values"`
	}
	if !decodeJSON(w, r, &input) {
		return
	}
	oldName := preset.Name
	if input.Name != nil {
		preset.Name = strings.TrimSpace(*input.Name)
	}
	if input.Description != nil {
		preset.Description = *input.Description
	}
	if err := validatePresetMetadata(preset.Name, preset.Description); err != nil {
		writeError(w, http.StatusBadRequest, "invalid_preset", err.Error())
		return
	}
	tx, err := a.db.BeginTx(r.Context(), nil)
	if err != nil {
		writeError(w, http.StatusInternalServerError, "preset_failed", "Could not update this preset.")
		return
	}
	defer tx.Rollback()
	fields, err := loadFieldsFrom(r.Context(), tx, item.ObjectTypeID, false)
	if err != nil {
		writeError(w, http.StatusInternalServerError, "database_error", "Could not load object fields.")
		return
	}
	oldValues := preset.Values
	if input.Values != nil {
		preset.Values, err = validatePresetValues(r.Context(), tx, item, fields, input.Values)
		if err != nil {
			writeValidationError(w, err)
			return
		}
	}
	encoded, err := json.Marshal(preset.Values)
	if err != nil || len(encoded) > maxPresetBytes {
		writeError(w, http.StatusRequestEntityTooLarge, "preset_too_large", "Preset overrides exceed the 1 MB limit.")
		return
	}
	preset.UpdatedAt = time.Now().UTC().Format(time.RFC3339Nano)
	if _, err := tx.ExecContext(r.Context(), `UPDATE object_presets SET name=?,description=?,values_json=?,updated_at=? WHERE id=? AND object_id=? AND game_id=?`, preset.Name, preset.Description, string(encoded), preset.UpdatedAt, preset.ID, item.ID, item.GameID); err != nil {
		writeError(w, http.StatusInternalServerError, "preset_failed", "Could not update this preset.")
		return
	}
	changes := presetHistoryChanges(preset.Name, fields, oldValues, preset.Values)
	if oldName != preset.Name {
		changes = append(changes, objectHistoryChange{FieldName: "Preset renamed: " + oldName, FieldType: "preset", Before: marshalRaw(oldName), After: marshalRaw(preset.Name)})
	}
	if len(changes) > 0 {
		if err := a.writeObjectHistory(r.Context(), tx, a.currentUser(r).ID, item, "updated", "api", historyTime(time.Now()), changes); err != nil {
			writeError(w, http.StatusInternalServerError, "preset_failed", "Could not record preset history.")
			return
		}
	}
	if err := tx.Commit(); err != nil {
		writeError(w, http.StatusInternalServerError, "preset_failed", "Could not update this preset.")
		return
	}
	writeJSON(w, http.StatusOK, preset)
}

func (a *API) duplicateObjectPreset(w http.ResponseWriter, r *http.Request) {
	item, source, err := a.loadObjectPreset(r, r.PathValue("presetID"))
	if err != nil {
		notFoundOrError(w, err, "Preset")
		return
	}
	tx, err := a.db.BeginTx(r.Context(), nil)
	if err != nil {
		writeError(w, http.StatusInternalServerError, "preset_failed", "Could not duplicate this preset.")
		return
	}
	defer tx.Rollback()
	var count, order int
	if err := tx.QueryRowContext(r.Context(), `SELECT COUNT(*),COALESCE(MAX(sort_order),-1)+1 FROM object_presets WHERE object_id=?`, item.ID).Scan(&count, &order); err != nil {
		writeError(w, http.StatusInternalServerError, "database_error", "Could not validate the preset limit.")
		return
	}
	if count >= maxObjectPresets {
		writeError(w, http.StatusConflict, "preset_limit", "An object can have at most 100 presets.")
		return
	}
	name := source.Name + " copy"
	if len(name) > 80 {
		name = strings.TrimSpace(source.Name[:75]) + " copy"
	}
	id, err := randomID()
	if err != nil {
		writeError(w, http.StatusInternalServerError, "preset_failed", "Could not duplicate this preset.")
		return
	}
	now := time.Now().UTC().Format(time.RFC3339Nano)
	encoded, _ := json.Marshal(source.Values)
	if _, err := tx.ExecContext(r.Context(), `INSERT INTO object_presets(id,object_id,game_id,name,description,sort_order,values_json,created_at,updated_at) VALUES(?,?,?,?,?,?,?,?,?)`, id, item.ID, item.GameID, name, source.Description, order, string(encoded), now, now); err != nil {
		writeError(w, http.StatusInternalServerError, "preset_failed", "Could not duplicate this preset.")
		return
	}
	fields, err := loadFieldsFrom(r.Context(), tx, item.ObjectTypeID, false)
	if err != nil {
		writeError(w, http.StatusInternalServerError, "database_error", "Could not load object fields.")
		return
	}
	changes := presetHistoryChanges(name, fields, nil, source.Values)
	changes = append(changes, objectHistoryChange{FieldName: "Preset created: " + name, FieldType: "preset", After: marshalRaw(name)})
	if err := a.writeObjectHistory(r.Context(), tx, a.currentUser(r).ID, item, "updated", "api", historyTime(time.Now()), changes); err != nil {
		writeError(w, http.StatusInternalServerError, "preset_failed", "Could not record preset history.")
		return
	}
	if err := tx.Commit(); err != nil {
		writeError(w, http.StatusInternalServerError, "preset_failed", "Could not duplicate this preset.")
		return
	}
	writeJSON(w, http.StatusCreated, objectPreset{ID: id, ObjectID: item.ID, Name: name, Description: source.Description, SortOrder: order, Values: source.Values, CreatedAt: now, UpdatedAt: now})
}

func (a *API) deleteObjectPreset(w http.ResponseWriter, r *http.Request) {
	item, preset, err := a.loadObjectPreset(r, r.PathValue("presetID"))
	if err != nil {
		notFoundOrError(w, err, "Preset")
		return
	}
	tx, err := a.db.BeginTx(r.Context(), nil)
	if err != nil {
		writeError(w, http.StatusInternalServerError, "preset_failed", "Could not delete this preset.")
		return
	}
	defer tx.Rollback()
	if _, err := tx.ExecContext(r.Context(), `DELETE FROM object_presets WHERE id=? AND object_id=? AND game_id=?`, preset.ID, item.ID, item.GameID); err != nil {
		writeError(w, http.StatusInternalServerError, "preset_failed", "Could not delete this preset.")
		return
	}
	fields, err := loadFieldsFrom(r.Context(), tx, item.ObjectTypeID, true)
	if err != nil {
		writeError(w, http.StatusInternalServerError, "database_error", "Could not load object fields.")
		return
	}
	changes := presetHistoryChanges(preset.Name, fields, preset.Values, nil)
	changes = append(changes, objectHistoryChange{FieldName: "Preset deleted: " + preset.Name, FieldType: "preset", Before: marshalRaw(preset.Name)})
	if err := a.writeObjectHistory(r.Context(), tx, a.currentUser(r).ID, item, "updated", "api", historyTime(time.Now()), changes); err != nil {
		writeError(w, http.StatusInternalServerError, "preset_failed", "Could not record preset history.")
		return
	}
	if err := tx.Commit(); err != nil {
		writeError(w, http.StatusInternalServerError, "preset_failed", "Could not delete this preset.")
		return
	}
	w.WriteHeader(http.StatusNoContent)
}

func (a *API) loadObjectPreset(r *http.Request, presetID string) (objectRecord, objectPreset, error) {
	var objectID string
	var preset objectPreset
	var raw string
	err := a.db.QueryRowContext(r.Context(), `SELECT p.id,p.object_id,p.name,p.description,p.sort_order,p.values_json,p.created_at,p.updated_at FROM object_presets p JOIN objects o ON o.id=p.object_id JOIN games g ON g.id=p.game_id AND g.id=o.game_id WHERE p.id=? AND g.user_id=? AND g.archived=0`, presetID, a.currentUser(r).ID).Scan(&preset.ID, &objectID, &preset.Name, &preset.Description, &preset.SortOrder, &raw, &preset.CreatedAt, &preset.UpdatedAt)
	if err != nil {
		return objectRecord{}, objectPreset{}, err
	}
	if err := json.Unmarshal([]byte(raw), &preset.Values); err != nil {
		return objectRecord{}, objectPreset{}, err
	}
	if preset.Values == nil {
		preset.Values = map[string]json.RawMessage{}
	}
	preset.ObjectID = objectID
	item, err := a.loadObjectAny(r, objectID)
	return item, preset, err
}

func scanObjectPreset(row interface{ Scan(...any) error }) (objectPreset, error) {
	var preset objectPreset
	var raw string
	if err := row.Scan(&preset.ID, &preset.ObjectID, &preset.Name, &preset.Description, &preset.SortOrder, &raw, &preset.CreatedAt, &preset.UpdatedAt); err != nil {
		return objectPreset{}, err
	}
	if err := json.Unmarshal([]byte(raw), &preset.Values); err != nil {
		return objectPreset{}, err
	}
	if preset.Values == nil {
		preset.Values = map[string]json.RawMessage{}
	}
	return preset, nil
}

func validatePresetMetadata(name, description string) error {
	if name == "" || len(name) > 80 {
		return validationError{code: "invalid_preset", message: "Preset name must be between 1 and 80 characters."}
	}
	if len(description) > 500 {
		return validationError{code: "invalid_preset", message: "Preset description must be 500 characters or fewer."}
	}
	return nil
}

func validatePresetValues(ctx context.Context, db dbExecutor, item objectRecord, fields []fieldDefinition, input map[string]json.RawMessage) (map[string]json.RawMessage, error) {
	if input == nil {
		input = map[string]json.RawMessage{}
	}
	fieldByID := make(map[string]fieldDefinition, len(fields))
	for _, field := range fields {
		fieldByID[field.ID] = field
	}
	keys := make([]string, 0, len(input))
	for fieldID := range input {
		keys = append(keys, fieldID)
	}
	sort.Strings(keys)
	values := make(map[string]json.RawMessage, len(input))
	for _, fieldID := range keys {
		field, ok := fieldByID[fieldID]
		if !ok {
			return nil, validationError{code: "unknown_field", message: "A preset value refers to an unknown or archived field."}
		}
		if field.FieldType == "formula" {
			return nil, validationError{code: "formula_read_only", message: "Formula fields are calculated and cannot be overridden by presets."}
		}
		validated, refs, _, err := validateFieldValue(field, input[fieldID])
		if err != nil {
			return nil, err
		}
		if field.FieldType == "reference" || field.FieldType == "references" {
			var options struct {
				AllowedTypeID string `json:"allowed_type_id"`
			}
			_ = json.Unmarshal(field.Options, &options)
			for _, ref := range refs {
				var targetType string
				var archived int
				if err := db.QueryRowContext(ctx, `SELECT o.object_type_id,CASE WHEN o.archived=1 OR t.archived=1 THEN 1 ELSE 0 END FROM objects o JOIN object_types t ON t.id=o.object_type_id WHERE o.id=? AND o.game_id=?`, ref, item.GameID).Scan(&targetType, &archived); err != nil || archived != 0 {
					return nil, validationError{code: "invalid_reference", message: "Preset references must point to active objects in the same game."}
				}
				if options.AllowedTypeID != "" && options.AllowedTypeID != targetType {
					return nil, validationError{code: "invalid_reference_type", message: "This field only accepts objects of the configured type."}
				}
			}
			if field.FieldType == "reference" {
				if len(refs) == 0 {
					values[fieldID] = json.RawMessage("null")
				} else {
					values[fieldID], _ = json.Marshal(refs[0])
				}
			} else {
				values[fieldID], _ = json.Marshal(refs)
			}
			continue
		}
		if string(input[fieldID]) == "null" {
			values[fieldID] = json.RawMessage("null")
		} else if validated != nil {
			values[fieldID] = validated
		}
	}
	return values, nil
}

func countFieldPresetOverrides(ctx context.Context, db dbExecutor, gameID, fieldID string) (int, error) {
	rows, err := db.QueryContext(ctx, `SELECT values_json FROM object_presets WHERE game_id=? ORDER BY id`, gameID)
	if err != nil {
		return 0, err
	}
	count := 0
	for rows.Next() {
		var raw string
		if err := rows.Scan(&raw); err != nil {
			rows.Close()
			return 0, err
		}
		var values map[string]json.RawMessage
		if err := json.Unmarshal([]byte(raw), &values); err != nil {
			rows.Close()
			return 0, err
		}
		if _, ok := values[fieldID]; ok {
			count++
		}
	}
	if err := rows.Err(); err != nil {
		rows.Close()
		return 0, err
	}
	if err := rows.Close(); err != nil {
		return 0, err
	}
	return count, nil
}

func mergePresetValues(canonical, overrides map[string]json.RawMessage) map[string]json.RawMessage {
	effective := make(map[string]json.RawMessage, len(canonical)+len(overrides))
	for id, value := range canonical {
		effective[id] = append(json.RawMessage(nil), value...)
	}
	for id, value := range overrides {
		effective[id] = append(json.RawMessage(nil), value...)
	}
	return effective
}

func presetHistoryChanges(name string, fields []fieldDefinition, before, after map[string]json.RawMessage) []objectHistoryChange {
	fieldByID := make(map[string]fieldDefinition, len(fields))
	for _, field := range fields {
		fieldByID[field.ID] = field
	}
	ids := make(map[string]bool, len(before)+len(after))
	for id := range before {
		ids[id] = true
	}
	for id := range after {
		ids[id] = true
	}
	keys := make([]string, 0, len(ids))
	for id := range ids {
		keys = append(keys, id)
	}
	sort.Strings(keys)
	changes := []objectHistoryChange{}
	for _, id := range keys {
		oldValue, oldOK := before[id]
		newValue, newOK := after[id]
		if oldOK == newOK && string(oldValue) == string(newValue) {
			continue
		}
		fieldName := id
		fieldType := "preset"
		if field, ok := fieldByID[id]; ok {
			fieldName = field.Name
			fieldType = "preset"
		}
		changes = append(changes, objectHistoryChange{FieldName: "Preset “" + name + "” · " + fieldName, FieldType: fieldType, Before: cloneRaw(oldValue, oldOK), After: cloneRaw(newValue, newOK)})
	}
	return changes
}

func marshalRaw(value any) json.RawMessage { raw, _ := json.Marshal(value); return raw }

func mergeOverrideIDs(raw json.RawMessage) ([]string, error) {
	if len(raw) == 0 || string(raw) == "null" {
		return []string{}, nil
	}
	var one string
	if json.Unmarshal(raw, &one) == nil {
		if one == "" {
			return []string{}, nil
		}
		return []string{one}, nil
	}
	var many []string
	if err := json.Unmarshal(raw, &many); err != nil {
		return nil, errors.New("invalid preset relationship override")
	}
	return many, nil
}
