package httpapi

import (
	"context"
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"fmt"
	"net/http"
)

type fieldTypeChangeRequest struct {
	TargetType   string                     `json:"target_type"`
	Options      optionalRawMessage         `json:"options"`
	Policy       map[string]json.RawMessage `json:"policy"`
	DefaultValue optionalRawMessage         `json:"default_value"`
	PreviewToken string                     `json:"preview_token"`
}

type fieldMigrationExample struct {
	ObjectID   string          `json:"object_id"`
	ObjectName string          `json:"object_name"`
	Value      json.RawMessage `json:"value"`
	Reason     string          `json:"reason"`
}

type fieldMigrationSummary struct {
	FieldID                 string                  `json:"field_id"`
	FieldName               string                  `json:"field_name"`
	CurrentType             string                  `json:"current_type"`
	TargetType              string                  `json:"target_type"`
	AffectedObjects         int                     `json:"affected_objects"`
	SavedValues             int                     `json:"saved_values"`
	CompatibleValues        int                     `json:"compatible_values"`
	IncompatibleValues      int                     `json:"incompatible_values"`
	ArchivedObjectsAffected int                     `json:"archived_objects_affected"`
	PresetOverrides         int                     `json:"preset_overrides"`
	CurrentDefault          json.RawMessage         `json:"current_default,omitempty"`
	ConvertedDefault        json.RawMessage         `json:"converted_default,omitempty"`
	DefaultError            string                  `json:"default_error,omitempty"`
	FormulaWarning          string                  `json:"formula_warning,omitempty"`
	Dependencies            []string                `json:"dependencies"`
	DependencyConflicts     []string                `json:"dependency_conflicts"`
	Blockers                []string                `json:"blockers"`
	Examples                []fieldMigrationExample `json:"incompatible_examples"`
	PreviewToken            string                  `json:"preview_token"`
	CanApply                bool                    `json:"can_apply"`
}

func (a *API) previewFieldTypeChange(w http.ResponseWriter, r *http.Request) {
	var input fieldTypeChangeRequest
	if !decodeJSON(w, r, &input) {
		return
	}
	tx, err := a.db.BeginTx(r.Context(), nil)
	if err != nil {
		writeError(w, http.StatusInternalServerError, "database_error", "Could not inspect the field migration.")
		return
	}
	defer tx.Rollback()
	field, gameID, err := loadFieldForUser(r.Context(), tx, a.currentUser(r).ID, r.PathValue("fieldID"))
	if err != nil {
		notFoundOrError(w, err, "Field")
		return
	}
	summary, err := a.analyzeFieldTypeChange(r.Context(), tx, field, gameID, input)
	if err != nil {
		writeError(w, http.StatusInternalServerError, "database_error", "Could not inspect the field migration.")
		return
	}
	if err := tx.Commit(); err != nil {
		writeError(w, http.StatusInternalServerError, "database_error", "Could not inspect the field migration.")
		return
	}
	writeJSON(w, http.StatusOK, summary)
}

func (a *API) applyFieldTypeChange(w http.ResponseWriter, r *http.Request) {
	var input fieldTypeChangeRequest
	if !decodeJSON(w, r, &input) {
		return
	}
	if input.PreviewToken == "" {
		writeError(w, http.StatusBadRequest, "preview_required", "Preview the conversion before applying it.")
		return
	}
	tx, err := a.db.BeginTx(r.Context(), nil)
	if err != nil {
		writeError(w, http.StatusInternalServerError, "migration_failed", "Could not start the field migration.")
		return
	}
	defer tx.Rollback()
	field, gameID, err := loadFieldForUser(r.Context(), tx, a.currentUser(r).ID, r.PathValue("fieldID"))
	if err != nil {
		notFoundOrError(w, err, "Field")
		return
	}
	summary, err := a.analyzeFieldTypeChange(r.Context(), tx, field, gameID, input)
	if err != nil {
		writeError(w, http.StatusInternalServerError, "database_error", "Could not validate the field migration.")
		return
	}
	if input.PreviewToken != summary.PreviewToken {
		writeJSON(w, http.StatusConflict, map[string]any{"error": map[string]string{"code": "stale_preview", "message": "Field values or dependencies changed after this preview. Review a fresh preview before applying the migration."}, "preview": summary})
		return
	}
	if !summary.CanApply {
		writeJSON(w, http.StatusConflict, map[string]any{"error": map[string]string{"code": "migration_blocked", "message": "This field type change needs a safe resolution before it can be applied."}, "preview": summary})
		return
	}
	if err := a.applyFieldTypeMigration(r.Context(), tx, field, gameID, a.currentUser(r).ID, input, summary); err != nil {
		if validation, ok := err.(validationError); ok {
			writeError(w, http.StatusConflict, validation.code, validation.message)
		} else {
			writeError(w, http.StatusInternalServerError, "migration_failed", "The field migration failed and was rolled back.")
		}
		return
	}
	if err := tx.Commit(); err != nil {
		writeError(w, http.StatusInternalServerError, "migration_failed", "The field migration failed and was rolled back.")
		return
	}
	field.FieldType = input.TargetType
	field.Options, err = migrationOptions(field, input)
	if err != nil {
		writeError(w, http.StatusInternalServerError, "migration_failed", "The field changed but its response could not be prepared.")
		return
	}
	if input.DefaultValue.Present {
		if string(input.DefaultValue.Value) == "null" {
			field.DefaultValue = nil
		} else {
			field.DefaultValue = input.DefaultValue.Value
		}
	} else if len(field.DefaultValue) > 0 {
		field.DefaultValue = summary.ConvertedDefault
	}
	writeJSON(w, http.StatusOK, field)
}

func loadFieldForUser(ctx context.Context, db dbExecutor, userID, fieldID string) (fieldDefinition, string, error) {
	var field fieldDefinition
	var gameID string
	var defaultJSON, optionsJSON []byte
	var archived int
	err := db.QueryRowContext(ctx, `SELECT f.id,f.object_type_id,f.name,f.field_type,f.icon,f.description,f.default_json,f.options_json,f.sort_order,f.archived,t.game_id FROM field_definitions f JOIN object_types t ON t.id=f.object_type_id JOIN games g ON g.id=t.game_id WHERE f.id=? AND g.user_id=? AND g.archived=0`, fieldID, userID).Scan(&field.ID, &field.ObjectTypeID, &field.Name, &field.FieldType, &field.Icon, &field.Description, &defaultJSON, &optionsJSON, &field.SortOrder, &archived, &gameID)
	if err != nil {
		return field, "", err
	}
	field.DefaultValue = append(json.RawMessage(nil), defaultJSON...)
	field.Options = append(json.RawMessage(nil), optionsJSON...)
	field.Archived = archived != 0
	return field, gameID, nil
}

func (a *API) analyzeFieldTypeChange(ctx context.Context, db dbExecutor, current fieldDefinition, gameID string, input fieldTypeChangeRequest) (fieldMigrationSummary, error) {
	result := fieldMigrationSummary{
		FieldID: current.ID, FieldName: current.Name, CurrentType: current.FieldType, TargetType: input.TargetType,
		Dependencies: []string{}, DependencyConflicts: []string{}, Blockers: []string{}, Examples: []fieldMigrationExample{},
	}
	if len(current.DefaultValue) > 0 && string(current.DefaultValue) != "null" {
		result.CurrentDefault = append(json.RawMessage(nil), current.DefaultValue...)
	}
	if !supportedFieldTypes[input.TargetType] {
		result.Blockers = append(result.Blockers, "Choose a supported target field type.")
	}
	if input.TargetType == current.FieldType {
		result.Blockers = append(result.Blockers, "Choose a type different from the current field type.")
	}
	targetOptions, err := migrationOptions(current, input)
	if err != nil {
		result.Blockers = append(result.Blockers, err.Error())
	}
	target := current
	target.FieldType = input.TargetType
	target.Options = targetOptions
	if err == nil {
		if optionErr := validateFieldOptions(target.FieldType, target.Options); optionErr != nil {
			result.Blockers = append(result.Blockers, "Target options: "+optionErr.Error())
		} else if optionErr = validateMigrationTypeOptions(ctx, db, gameID, target.FieldType, target.Options); optionErr != nil {
			result.Blockers = append(result.Blockers, "Target options: "+optionErr.Error())
		}
	}
	if input.TargetType == "formula" {
		result.Blockers = append(result.Blockers, "Numeric-to-formula migration needs formula editing and dependency validation. Create a new formula field and keep this field unchanged for now.")
	}
	if current.FieldType == "formula" && !numericType(input.TargetType) {
		result.Blockers = append(result.Blockers, "A calculated field can only be frozen into a numeric field.")
	}
	if current.FieldType == "formula" && numericType(input.TargetType) {
		if !policyBool(input.Policy, "freeze_formula") {
			result.Blockers = append(result.Blockers, "Confirm that the calculated formula result should be frozen into stored values.")
		}
		result.FormulaWarning = "Future changes to this formula's inputs will no longer update the migrated values."
	}
	if input.DefaultValue.Present {
		if string(input.DefaultValue.Value) == "null" {
			target.DefaultValue = nil
		} else {
			target.DefaultValue = append(json.RawMessage(nil), input.DefaultValue.Value...)
		}
	} else if len(current.DefaultValue) > 0 && string(current.DefaultValue) != "null" {
		converted, convertErr := convertFieldTypeValue(current.FieldType, target, input.Policy, current.DefaultValue)
		if convertErr != nil {
			result.DefaultError = convertErr.Error()
		} else {
			target.DefaultValue = converted
			result.ConvertedDefault = append(json.RawMessage(nil), converted...)
		}
	}
	if input.DefaultValue.Present && len(target.DefaultValue) > 0 {
		if err := validateDefaultValue(target.FieldType, target.Options, target.DefaultValue); err != nil {
			result.DefaultError = err.Error()
		} else {
			result.ConvertedDefault = append(json.RawMessage(nil), target.DefaultValue...)
		}
	}
	if input.DefaultValue.Present && string(input.DefaultValue.Value) == "null" {
		result.ConvertedDefault = nil
	}
	if result.DefaultError != "" {
		result.Blockers = append(result.Blockers, "Default value: "+result.DefaultError)
	}
	deps, err := a.fieldDependencies(ctx, db, current.ID)
	if err != nil {
		return result, err
	}
	var primaryCount int
	if err := db.QueryRowContext(ctx, `SELECT COUNT(*) FROM object_types WHERE primary_display_field_id=?`, current.ID).Scan(&primaryCount); err != nil {
		return result, err
	}
	if primaryCount > 0 {
		deps = append(deps, "primary display field")
	}
	result.Dependencies = deps
	result.PresetOverrides, err = countFieldPresetOverrides(ctx, db, gameID, current.ID)
	if err != nil {
		return result, err
	}
	if result.PresetOverrides > 0 {
		result.Dependencies = append(result.Dependencies, "preset overrides")
		result.Blockers = append(result.Blockers, "Remove or safely convert preset overrides before changing this field type.")
	}
	blockingDependencies := append([]string(nil), deps...)
	if policyString(input.Policy, "relationship_strategy") == "serialize_ids" && (target.FieldType == "text" || target.FieldType == "long_text") {
		filtered := blockingDependencies[:0]
		for _, dependency := range blockingDependencies {
			if dependency != "object relationships" {
				filtered = append(filtered, dependency)
			}
		}
		blockingDependencies = filtered
	}
	if !sameMigrationFamily(current.FieldType, target.FieldType) && len(blockingDependencies) > 0 {
		result.DependencyConflicts = append(result.DependencyConflicts, blockingDependencies...)
		result.Blockers = append(result.Blockers, "Resolve or remove dependent formulas, goals, object views, or display settings before changing to a different value family.")
	}
	if current.FieldType == "formula" && numericType(target.FieldType) {
		if err := a.inspectFormulaFieldValues(ctx, db, gameID, current, target, input.Policy, &result); err != nil {
			return result, err
		}
	} else if current.FieldType == "formula" || target.FieldType == "formula" {
		// Unsupported formula conversions remain blocked with an explicit reason.
	} else {
		if err := a.inspectStoredFieldValues(ctx, db, gameID, current, target, input.Policy, &result); err != nil {
			return result, err
		}
		if err := a.inspectFieldRelationships(ctx, db, current, target, input.Policy, &result); err != nil {
			return result, err
		}
	}
	result.CompatibleValues = result.SavedValues - result.IncompatibleValues
	if result.CompatibleValues < 0 {
		result.CompatibleValues = 0
	}
	defaultsPresent := len(current.DefaultValue) > 0 && string(current.DefaultValue) != "null" || len(target.DefaultValue) > 0 && string(target.DefaultValue) != "null"
	if current.FieldType != "formula" && target.FieldType != "formula" {
		if err := db.QueryRowContext(ctx, `SELECT COUNT(*) FROM objects o WHERE o.object_type_id=? AND (EXISTS(SELECT 1 FROM field_values v WHERE v.object_id=o.id AND v.field_id=?) OR EXISTS(SELECT 1 FROM object_relationships rel WHERE rel.object_id=o.id AND rel.field_id=?) OR (?=1))`, current.ObjectTypeID, current.ID, current.ID, boolInt(defaultsPresent)).Scan(&result.AffectedObjects); err != nil {
			return result, err
		}
	} else if current.FieldType == "formula" {
		if err := db.QueryRowContext(ctx, `SELECT COUNT(*) FROM objects WHERE object_type_id=?`, current.ObjectTypeID).Scan(&result.AffectedObjects); err != nil {
			return result, err
		}
	}
	if err := db.QueryRowContext(ctx, `SELECT COUNT(*) FROM objects o WHERE o.object_type_id=? AND o.archived=1 AND (EXISTS(SELECT 1 FROM field_values v WHERE v.object_id=o.id AND v.field_id=?) OR EXISTS(SELECT 1 FROM object_relationships rel WHERE rel.object_id=o.id AND rel.field_id=?) OR (?=1))`, current.ObjectTypeID, current.ID, current.ID, boolInt(defaultsPresent)).Scan(&result.ArchivedObjectsAffected); err != nil {
		return result, err
	}
	if current.FieldType == "formula" {
		if err := db.QueryRowContext(ctx, `SELECT COUNT(*) FROM objects WHERE object_type_id=? AND archived=1`, current.ObjectTypeID).Scan(&result.ArchivedObjectsAffected); err != nil {
			return result, err
		}
	}
	revision, err := fieldMigrationRevision(ctx, db, current.ObjectTypeID, current.ID)
	if err != nil {
		return result, err
	}
	result.PreviewToken, err = fieldMigrationToken(revision, input)
	if err != nil {
		return result, err
	}
	result.CanApply = len(result.Blockers) == 0 && result.IncompatibleValues == 0 && result.DefaultError == "" && input.TargetType != current.FieldType && supportedFieldTypes[input.TargetType]
	return result, nil
}

func migrationOptions(current fieldDefinition, input fieldTypeChangeRequest) (json.RawMessage, error) {
	if input.Options.Present {
		if !json.Valid(input.Options.Value) {
			return nil, fmt.Errorf("Target options must be valid JSON.")
		}
		return append(json.RawMessage(nil), input.Options.Value...), nil
	}
	if (current.FieldType == "reference" || current.FieldType == "references") && (input.TargetType == "reference" || input.TargetType == "references") {
		return append(json.RawMessage(nil), current.Options...), nil
	}
	return json.RawMessage(`{}`), nil
}

func validateMigrationTypeOptions(ctx context.Context, db dbExecutor, gameID, fieldType string, raw json.RawMessage) error {
	if fieldType != "reference" && fieldType != "references" {
		return nil
	}
	var options struct {
		AllowedTypeID string `json:"allowed_type_id"`
	}
	if err := json.Unmarshal(raw, &options); err != nil {
		return fmt.Errorf("Reference options must be valid JSON.")
	}
	if options.AllowedTypeID == "" {
		return nil
	}
	var count int
	if err := db.QueryRowContext(ctx, `SELECT COUNT(*) FROM object_types WHERE id=? AND game_id=?`, options.AllowedTypeID, gameID).Scan(&count); err != nil || count != 1 {
		return fmt.Errorf("Allowed target type must belong to the same game.")
	}
	return nil
}

func sameMigrationFamily(from, to string) bool {
	if from == to {
		return true
	}
	numeric := func(value string) bool {
		return value == "integer" || value == "decimal" || value == "percentage" || value == "formula"
	}
	textual := func(value string) bool { return value == "text" || value == "long_text" }
	reference := func(value string) bool { return value == "reference" || value == "references" }
	return (numeric(from) && numeric(to)) || (textual(from) && textual(to)) || (reference(from) && reference(to))
}

func fieldMigrationToken(revision string, input fieldTypeChangeRequest) (string, error) {
	options := any(json.RawMessage(`{}`))
	if input.Options.Present {
		options = input.Options.Value
	}
	var defaultValue any
	if input.DefaultValue.Present {
		defaultValue = input.DefaultValue.Value
	}
	policy := input.Policy
	if policy == nil {
		policy = map[string]json.RawMessage{}
	}
	canonical, err := json.Marshal(struct {
		Revision string                     `json:"revision"`
		Target   string                     `json:"target"`
		Options  any                        `json:"options"`
		Policy   map[string]json.RawMessage `json:"policy"`
		Default  any                        `json:"default"`
	}{revision, input.TargetType, options, policy, defaultValue})
	if err != nil {
		return "", err
	}
	sum := sha256.Sum256(canonical)
	return hex.EncodeToString(sum[:]), nil
}

func fieldMigrationRevision(ctx context.Context, db dbExecutor, typeID, fieldID string) (string, error) {
	hash := sha256.New()
	queries := []struct {
		query string
		args  []any
	}{
		{`SELECT id,name,field_type,COALESCE(default_json,''),options_json,archived FROM field_definitions WHERE id=?`, []any{fieldID}},
		{`SELECT object_id,value_json FROM field_values WHERE field_id=? ORDER BY object_id`, []any{fieldID}},
		{`SELECT object_id,referenced_object_id FROM object_relationships WHERE field_id=? ORDER BY object_id,referenced_object_id`, []any{fieldID}},
		{`SELECT id,archived FROM objects WHERE object_type_id=? ORDER BY id`, []any{typeID}},
		{`SELECT object_id,field_id,value_json FROM field_values WHERE object_type_id IN (SELECT id FROM object_types WHERE game_id=(SELECT game_id FROM object_types WHERE id=?)) ORDER BY object_id,field_id`, []any{typeID}},
		{`SELECT object_id,field_id,referenced_object_id FROM object_relationships WHERE game_id=(SELECT game_id FROM object_types WHERE id=?) ORDER BY object_id,field_id,referenced_object_id`, []any{typeID}},
		{`SELECT o.id,o.object_type_id,o.archived FROM objects o WHERE o.game_id=(SELECT game_id FROM object_types WHERE id=?) ORDER BY o.id`, []any{typeID}},
		{`SELECT f.id,f.object_type_id,f.field_type,f.options_json,f.archived FROM field_definitions f JOIN object_types t ON t.id=f.object_type_id WHERE t.game_id=(SELECT game_id FROM object_types WHERE id=?) ORDER BY f.id`, []any{typeID}},
		{`SELECT id,goal_type,field_id,object_id,COALESCE(target_json,'') FROM goals WHERE game_id=(SELECT game_id FROM object_types WHERE id=?) ORDER BY id`, []any{typeID}},
		{`SELECT id,archived,display_config FROM sections WHERE game_id=(SELECT game_id FROM object_types WHERE id=?) ORDER BY id`, []any{typeID}},
		{`SELECT id,primary_display_field_id FROM object_types WHERE game_id=(SELECT game_id FROM object_types WHERE id=?) ORDER BY id`, []any{typeID}},
		{`SELECT id,object_id,name,values_json,updated_at FROM object_presets WHERE game_id=(SELECT game_id FROM object_types WHERE id=?) ORDER BY id`, []any{typeID}},
	}
	for _, item := range queries {
		if err := hashQuery(ctx, db, hash, item.query, item.args...); err != nil {
			return "", err
		}
	}
	return hex.EncodeToString(hash.Sum(nil)), nil
}

func hashQuery(ctx context.Context, db dbExecutor, hash interface{ Write([]byte) (int, error) }, query string, args ...any) error {
	rows, err := db.QueryContext(ctx, query, args...)
	if err != nil {
		return err
	}
	defer rows.Close()
	columns, err := rows.Columns()
	if err != nil {
		return err
	}
	values := make([]any, len(columns))
	destinations := make([]any, len(columns))
	for i := range values {
		destinations[i] = &values[i]
	}
	for rows.Next() {
		for i := range values {
			values[i] = nil
		}
		if err := rows.Scan(destinations...); err != nil {
			return err
		}
		for _, value := range values {
			encoded, err := json.Marshal(value)
			if err != nil {
				return err
			}
			var size [8]byte
			for i := range size {
				size[i] = byte(uint64(len(encoded)) >> (8 * i))
			}
			if _, err := hash.Write(size[:]); err != nil {
				return err
			}
			if _, err := hash.Write(encoded); err != nil {
				return err
			}
		}
		if _, err := hash.Write([]byte{0xff}); err != nil {
			return err
		}
	}
	return rows.Err()
}
