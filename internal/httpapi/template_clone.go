package httpapi

import (
	"context"
	"encoding/json"
	"fmt"
	"strings"
	"time"

	"gametracker/internal/formula"
)

func (a *API) instantiateTemplateData(ctx context.Context, userID string, source gameTemplate, document templateDocument, name, icon string) (game, error) {
	if err := validateTemplateDocument(document); err != nil {
		return game{}, validationError{code: "invalid_template", message: "This template's structure is invalid or unsupported."}
	}
	tx, err := a.db.BeginTx(ctx, nil)
	if err != nil {
		return game{}, err
	}
	defer tx.Rollback()
	gameID, err := randomID()
	if err != nil {
		return game{}, err
	}
	now := time.Now().UTC().Format(time.RFC3339Nano)
	gameIcon := safeTemplateIcon(icon, document.GameIcon)
	if _, err := tx.ExecContext(ctx, `INSERT INTO games(icon,id,user_id,name,description,cover_image,personal_notes,created_at,updated_at) VALUES(?,?,?,?,?,'','',?,?)`, gameIcon, gameID, userID, name, document.GameDescription, now, now); err != nil {
		return game{}, err
	}
	typeIDs := make(map[string]string, len(document.Types))
	fieldIDs := make(map[string]string)
	sectionIDs := make(map[string]string, len(document.Sections))
	for _, typ := range document.Types {
		newID, err := randomID()
		if err != nil {
			return game{}, err
		}
		typeIDs[typ.SourceID] = newID
		for _, field := range typ.Fields {
			fieldID, err := randomID()
			if err != nil {
				return game{}, err
			}
			fieldIDs[field.SourceID] = fieldID
		}
	}
	for _, section := range document.Sections {
		newID, err := randomID()
		if err != nil {
			return game{}, err
		}
		sectionIDs[section.SourceID] = newID
	}
	for _, typ := range document.Types {
		if _, err := tx.ExecContext(ctx, `INSERT INTO object_types(id,game_id,name,description,icon,image,sort_order,archived,created_at,updated_at) VALUES(?,?,?,?,?,'',?,?,?,?)`, typeIDs[typ.SourceID], gameID, typ.Name, typ.Description, safeTemplateIcon(typ.Icon, "◈"), typ.SortOrder, boolInt(typ.Archived), now, now); err != nil {
			return game{}, err
		}
	}
	for _, typ := range document.Types {
		for _, field := range typ.Fields {
			options, err := remapTemplateFieldOptions(field, fieldIDs, typeIDs)
			if err != nil {
				return game{}, validationError{code: "invalid_template", message: "A field definition refers to a missing template ID."}
			}
			if err := validateFieldOptions(field.FieldType, options); err != nil {
				return game{}, validationError{code: "invalid_template", message: "A field definition is invalid."}
			}
			if len(field.Default) > 0 {
				if err := validateDefaultValue(field.FieldType, options, field.Default); err != nil {
					return game{}, validationError{code: "invalid_template", message: "A field default is invalid."}
				}
			}
			if _, err := tx.ExecContext(ctx, `INSERT INTO field_definitions(id,object_type_id,name,field_type,icon,description,default_json,options_json,sort_order,archived,created_at,updated_at) VALUES(?,?,?,?,?,?,?,?,?,?,?,?)`, fieldIDs[field.SourceID], typeIDs[typ.SourceID], field.Name, field.FieldType, safeTemplateIcon(field.Icon, "✦"), field.Description, nullableJSON(field.Default), string(options), field.SortOrder, boolInt(field.Archived), now, now); err != nil {
				return game{}, err
			}
		}
	}
	for _, typ := range document.Types {
		if typ.PrimaryDisplayFieldID != "" {
			if _, err := tx.ExecContext(ctx, `UPDATE object_types SET primary_display_field_id=? WHERE id=?`, fieldIDs[typ.PrimaryDisplayFieldID], typeIDs[typ.SourceID]); err != nil {
				return game{}, err
			}
		}
	}
	for _, section := range document.Sections {
		displayConfig, err := remapTemplateJSON(section.DisplayConfig, fieldIDs, typeIDs)
		if err != nil {
			return game{}, validationError{code: "invalid_template", message: "A section setting refers to a missing template ID."}
		}
		if _, err := tx.ExecContext(ctx, `INSERT INTO sections(id,game_id,parent_id,name,icon,sort_order,display_style,display_config,archived,created_at,updated_at) VALUES(?,?,NULL,?,?,?,?,?,?,?,?)`, sectionIDs[section.SourceID], gameID, section.Name, safeTemplateIcon(section.Icon, "✦"), section.SortOrder, section.DisplayStyle, string(displayConfig), boolInt(section.Archived), now, now); err != nil {
			return game{}, err
		}
	}
	for _, section := range document.Sections {
		if section.ParentSourceID != "" {
			if _, err := tx.ExecContext(ctx, `UPDATE sections SET parent_id=? WHERE id=? AND game_id=?`, sectionIDs[section.ParentSourceID], sectionIDs[section.SourceID], gameID); err != nil {
				return game{}, err
			}
		}
	}
	if err := validateInstantiatedTemplateFormulas(ctx, tx, document, typeIDs, fieldIDs); err != nil {
		return game{}, validationError{code: "invalid_template", message: "A formula does not match the remapped field definitions."}
	}
	if err := tx.Commit(); err != nil {
		return game{}, err
	}
	return game{ID: gameID, Name: name, Icon: gameIcon, Description: document.GameDescription, CoverImage: "", PersonalNotes: "", CreatedAt: now, UpdatedAt: now}, nil
}

func remapTemplateFieldOptions(field templateField, fields, types map[string]string) (json.RawMessage, error) {
	if !json.Valid(field.Options) {
		return nil, fmt.Errorf("invalid field options")
	}
	var options map[string]json.RawMessage
	if err := json.Unmarshal(field.Options, &options); err != nil || options == nil {
		return nil, fmt.Errorf("invalid field options")
	}
	if raw := options["allowed_type_id"]; len(raw) > 0 {
		var sourceID string
		if json.Unmarshal(raw, &sourceID) == nil && sourceID != "" {
			newID, ok := types[sourceID]
			if !ok {
				return nil, fmt.Errorf("missing target type")
			}
			options["allowed_type_id"], _ = json.Marshal(newID)
		}
	}
	if field.FieldType == "formula" {
		var config struct {
			Expression  string                   `json:"expression"`
			Aggregation *relationshipAggregation `json:"aggregation"`
		}
		if json.Unmarshal(field.Options, &config) != nil {
			return nil, fmt.Errorf("invalid formula")
		}
		config.Expression = remapFormulaExpression(config.Expression, fields)
		options["expression"], _ = json.Marshal(config.Expression)
		if config.Aggregation != nil {
			var ok bool
			config.Aggregation.RelationshipFieldID, ok = fields[config.Aggregation.RelationshipFieldID]
			if !ok {
				return nil, fmt.Errorf("missing relationship field")
			}
			if config.Aggregation.FieldID != "" {
				config.Aggregation.FieldID, ok = fields[config.Aggregation.FieldID]
				if !ok {
					return nil, fmt.Errorf("missing aggregate field")
				}
			}
			options["aggregation"], _ = json.Marshal(config.Aggregation)
		}
	}
	return json.Marshal(options)
}

func remapFormulaExpression(expression string, fields map[string]string) string {
	for source, target := range fields {
		expression = strings.ReplaceAll(expression, "@{"+source+"}", "@{"+target+"}")
	}
	return expression
}

func remapTemplateJSON(raw json.RawMessage, fields, types map[string]string) (json.RawMessage, error) {
	if len(raw) == 0 {
		return json.RawMessage(`{}`), nil
	}
	decoder := json.NewDecoder(strings.NewReader(string(raw)))
	decoder.UseNumber()
	var value any
	if err := decoder.Decode(&value); err != nil {
		return nil, err
	}
	var walk func(any, string) error
	walk = func(node any, key string) error {
		switch item := node.(type) {
		case map[string]any:
			for childKey, child := range item {
				if childKey == "type_id" || childKey == "allowed_type_id" {
					if source, ok := child.(string); ok && source != "" {
						mapped, exists := types[source]
						if !exists {
							return fmt.Errorf("unknown type reference")
						}
						item[childKey] = mapped
						continue
					}
				}
				if childKey == "sort" {
					if source, ok := child.(string); ok {
						if mapped, exists := fields[source]; exists {
							item[childKey] = mapped
							continue
						}
					}
				}
				if (childKey == "field_id" || strings.HasSuffix(childKey, "_field_id")) && child != nil {
					if source, ok := child.(string); ok && source != "" {
						mapped, exists := fields[source]
						if !exists {
							return fmt.Errorf("unknown field reference")
						}
						item[childKey] = mapped
						continue
					}
				}
				if strings.HasSuffix(childKey, "_field_ids") {
					if ids, ok := child.([]any); ok {
						for index, value := range ids {
							source, ok := value.(string)
							if !ok {
								return fmt.Errorf("invalid field ID list")
							}
							mapped, exists := fields[source]
							if !exists {
								return fmt.Errorf("unknown field reference")
							}
							ids[index] = mapped
						}
						continue
					}
				}
				if err := walk(child, childKey); err != nil {
					return err
				}
			}
		case []any:
			for _, child := range item {
				if err := walk(child, key); err != nil {
					return err
				}
			}
		}
		return nil
	}
	if err := walk(value, ""); err != nil {
		return nil, err
	}
	return json.Marshal(value)
}

func validateInstantiatedTemplateFormulas(ctx context.Context, tx dbExecutor, document templateDocument, typeIDs, fieldIDs map[string]string) error {
	for _, typ := range document.Types {
		for _, field := range typ.Fields {
			if field.FieldType != "formula" {
				continue
			}
			var options struct {
				Expression string `json:"expression"`
			}
			if json.Unmarshal(field.Options, &options) != nil {
				return fmt.Errorf("invalid formula config")
			}
			mapped := remapFormulaExpression(options.Expression, fieldIDs)
			if err := formula.Validate(mapped); err != nil {
				return err
			}
			for _, oldID := range formula.References(options.Expression) {
				if oldID == "related" {
					continue
				}
				var kind, owner string
				if err := tx.QueryRowContext(ctx, `SELECT field_type,object_type_id FROM field_definitions WHERE id=?`, fieldIDs[oldID]).Scan(&kind, &owner); err != nil {
					return err
				}
				if owner != typeIDs[typ.SourceID] || (kind != "integer" && kind != "decimal" && kind != "percentage" && kind != "formula") {
					return fmt.Errorf("formula reference has wrong target")
				}
			}
		}
	}
	return nil
}
