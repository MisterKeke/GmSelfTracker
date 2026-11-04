package httpapi

import (
	"context"
	"encoding/json"
	"fmt"
	"time"
)

// Run in the same transaction as the schema update. Archived records and live
// goal targets count as existing data and must not be silently invalidated.
func migrateFieldOptions(ctx context.Context, db dbExecutor, field *fieldDefinition, replacements map[string]string, removeReferences bool) error {
	conflict := func(message string) error { return validationError{code: "schema_conflict", message: message} }
	if len(replacements) > 200 {
		return conflict("Use no more than 200 choice replacements.")
	}
	if field.FieldType == "select" {
		var options struct {
			Choices []string `json:"choices"`
		}
		_ = json.Unmarshal(field.Options, &options)
		choices := map[string]bool{}
		for _, choice := range options.Choices {
			choices[choice] = true
		}
		for _, replacement := range replacements {
			if !choices[replacement] {
				return conflict("Every replacement must be one of the new choices.")
			}
		}
		remap := func(raw json.RawMessage) (json.RawMessage, error) {
			var value string
			if json.Unmarshal(raw, &value) != nil {
				return nil, conflict("Select values must be text choices.")
			}
			if choices[value] {
				return raw, nil
			}
			replacement, ok := replacements[value]
			if !ok {
				return nil, conflict(fmt.Sprintf("Choice %q is still used by an object, default, or goal. Keep it or choose a replacement.", value))
			}
			encoded, _ := json.Marshal(replacement)
			return encoded, nil
		}
		if len(field.DefaultValue) > 0 && string(field.DefaultValue) != "null" {
			raw, err := remap(field.DefaultValue)
			if err != nil {
				return err
			}
			field.DefaultValue = raw
		}
		rows, err := db.QueryContext(ctx, `SELECT object_id,value_json FROM field_values WHERE field_id=?`, field.ID)
		if err != nil {
			return err
		}
		type update struct {
			id    string
			value json.RawMessage
		}
		updates := []update{}
		for rows.Next() {
			var id string
			var raw []byte
			if err := rows.Scan(&id, &raw); err != nil {
				rows.Close()
				return err
			}
			if string(raw) == "null" {
				continue
			}
			value, err := remap(raw)
			if err != nil {
				rows.Close()
				return err
			}
			if string(value) != string(raw) {
				updates = append(updates, update{id, value})
			}
		}
		if err := rows.Err(); err != nil {
			rows.Close()
			return err
		}
		rows.Close()
		for _, update := range updates {
			if _, err := db.ExecContext(ctx, `UPDATE field_values SET value_json=?,updated_at=? WHERE object_id=? AND field_id=?`, string(update.value), time.Now().UTC().Format(time.RFC3339), update.id, field.ID); err != nil {
				return err
			}
		}
		rows, err = db.QueryContext(ctx, `SELECT id,target_json FROM goals WHERE goal_type='field_target' AND game_id=(SELECT game_id FROM object_types WHERE id=?)`, field.ObjectTypeID)
		if err != nil {
			return err
		}
		goals := []update{}
		for rows.Next() {
			var id string
			var raw []byte
			if err := rows.Scan(&id, &raw); err != nil {
				rows.Close()
				return err
			}
			var rule goalRule
			if json.Unmarshal(raw, &rule) != nil || len(rule.Conditions) == 0 {
				continue
			}
			changed := false
			for i := range rule.Conditions {
				if rule.Conditions[i].FieldID == field.ID {
					value, err := remap(rule.Conditions[i].Value)
					if err != nil {
						rows.Close()
						return err
					}
					if string(value) != string(rule.Conditions[i].Value) {
						changed = true
					}
					rule.Conditions[i].Value = value
				}
			}
			if changed {
				value, err := json.Marshal(rule)
				if err != nil {
					rows.Close()
					return err
				}
				goals = append(goals, update{id, value})
			}
		}
		if err := rows.Err(); err != nil {
			rows.Close()
			return err
		}
		rows.Close()
		for _, update := range goals {
			if _, err := db.ExecContext(ctx, `UPDATE goals SET target_json=?,updated_at=? WHERE id=?`, string(update.value), time.Now().UTC().Format(time.RFC3339), update.id); err != nil {
				return err
			}
		}
	}
	if field.FieldType == "reference" || field.FieldType == "references" {
		var options struct {
			AllowedTypeID string `json:"allowed_type_id"`
		}
		_ = json.Unmarshal(field.Options, &options)
		if options.AllowedTypeID != "" {
			var incompatible int
			if err := db.QueryRowContext(ctx, `SELECT COUNT(*) FROM object_relationships r JOIN objects o ON o.id=r.referenced_object_id WHERE r.field_id=? AND o.object_type_id<>?`, field.ID, options.AllowedTypeID).Scan(&incompatible); err != nil {
				return err
			}
			if incompatible > 0 && !removeReferences {
				return conflict("Existing objects reference other types. Keep the allowed type, or explicitly remove incompatible references.")
			}
			var dependencies int
			err := db.QueryRowContext(ctx, `SELECT
			 (SELECT COUNT(*) FROM field_definitions f JOIN field_definitions target ON target.id=json_extract(f.options_json,'$.aggregation.field_id') WHERE f.archived=0 AND json_extract(f.options_json,'$.aggregation.relationship_field_id')=? AND json_extract(f.options_json,'$.aggregation.operation')<>'count' AND target.object_type_id<>?) +
			 (SELECT COUNT(*) FROM sections s JOIN field_definitions target ON target.id=json_extract(s.display_config,'$.progress_child_field_id') WHERE s.archived=0 AND json_extract(s.display_config,'$.progress_mode')='children' AND json_extract(s.display_config,'$.progress_relationship_field_id')=? AND target.object_type_id<>?)`, field.ID, options.AllowedTypeID, field.ID, options.AllowedTypeID).Scan(&dependencies)
			if err != nil {
				return err
			}
			if dependencies > 0 {
				return conflict("Update dependent relationship formulas or child progress views before changing the allowed type.")
			}
			if incompatible > 0 {
				if _, err := db.ExecContext(ctx, `DELETE FROM object_relationships WHERE field_id=? AND referenced_object_id IN (SELECT id FROM objects WHERE object_type_id<>?)`, field.ID, options.AllowedTypeID); err != nil {
					return err
				}
			}
		}
	}
	if len(field.DefaultValue) > 0 && string(field.DefaultValue) != "null" {
		if err := validateDefaultValue(field.FieldType, field.Options, field.DefaultValue); err != nil {
			return conflict(err.Error())
		}
	}
	return nil
}
