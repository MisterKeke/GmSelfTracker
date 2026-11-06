package httpapi

import (
	"context"
	"database/sql"
	"encoding/json"
	"fmt"
	"math"
)

// Only ordinary numeric fields may be aggregated. This keeps relationships
// cyclic without allowing recursive formula evaluation across objects.
type relationshipAggregation struct {
	RelationshipFieldID string `json:"relationship_field_id"`
	FieldID             string `json:"field_id"`
	Operation           string `json:"operation"`
}

func (a *API) validateAggregation(ctx context.Context, typeID string, config relationshipAggregation) error {
	var gameID, kind string
	var options []byte
	err := a.db.QueryRowContext(ctx, `SELECT t.game_id,f.field_type,f.options_json FROM field_definitions f JOIN object_types t ON t.id=f.object_type_id WHERE f.id=? AND t.id=? AND f.archived=0`, config.RelationshipFieldID, typeID).Scan(&gameID, &kind, &options)
	if err != nil || (kind != "reference" && kind != "references") {
		return fmt.Errorf("Choose an active relationship field on this type.")
	}
	if config.Operation != "sum" && config.Operation != "avg" && config.Operation != "min" && config.Operation != "max" && config.Operation != "count" {
		return fmt.Errorf("Choose sum, avg, min, max, or count.")
	}
	if config.Operation == "count" {
		return nil
	}
	var targetType string
	err = a.db.QueryRowContext(ctx, `SELECT f.field_type,f.object_type_id FROM field_definitions f JOIN object_types t ON t.id=f.object_type_id WHERE f.id=? AND t.game_id=? AND f.archived=0 AND t.archived=0`, config.FieldID, gameID).Scan(&kind, &targetType)
	if err != nil || (kind != "integer" && kind != "decimal" && kind != "percentage" && kind != "progress") {
		return fmt.Errorf("Aggregate an active ordinary numeric or progress field in this game.")
	}
	var referenceOptions struct {
		AllowedTypeID string `json:"allowed_type_id"`
	}
	_ = json.Unmarshal(options, &referenceOptions)
	if referenceOptions.AllowedTypeID != "" && referenceOptions.AllowedTypeID != targetType {
		return fmt.Errorf("The aggregated field must belong to the allowed target type.")
	}
	return nil
}

func (a *API) relationshipAggregate(ctx context.Context, db dbExecutor, item objectRecord, config relationshipAggregation) (float64, error) {
	rows, err := db.QueryContext(ctx, `SELECT o.id,COALESCE(v.value_json,f.default_json) FROM object_relationships r JOIN objects o ON o.id=r.referenced_object_id AND o.game_id=r.game_id JOIN object_types t ON t.id=o.object_type_id LEFT JOIN field_definitions f ON f.id=? AND f.object_type_id=o.object_type_id AND f.archived=0 LEFT JOIN field_values v ON v.object_id=o.id AND v.field_id=f.id WHERE r.object_id=? AND r.field_id=? AND r.game_id=? AND o.archived=0 AND t.archived=0`, config.FieldID, item.ID, config.RelationshipFieldID, item.GameID)
	if err != nil {
		return 0, err
	}
	defer rows.Close()
	count, result := 0, 0.0
	for rows.Next() {
		var id string
		var raw []byte
		if err := rows.Scan(&id, &raw); err != nil {
			return 0, err
		}
		if config.Operation == "count" {
			count++
			continue
		}
		value, err := numericValue(raw)
		if err != nil {
			continue
		}
		if count == 0 && (config.Operation == "min" || config.Operation == "max") {
			result = value
		} else {
			switch config.Operation {
			case "sum", "avg":
				result += value
			case "min":
				result = math.Min(result, value)
			case "max":
				result = math.Max(result, value)
			}
		}
		count++
	}
	if err := rows.Err(); err != nil {
		return 0, err
	}
	if config.Operation == "count" {
		return float64(count), nil
	}
	if config.Operation == "avg" || config.Operation == "min" || config.Operation == "max" {
		if count == 0 {
			return 0, fmt.Errorf("no linked numeric values")
		}
		if config.Operation == "avg" {
			result /= float64(count)
		}
	}
	if math.IsNaN(result) || math.IsInf(result, 0) {
		return 0, fmt.Errorf("aggregation result is not finite")
	}
	return result, nil
}

func (a *API) relationshipAggregateWithOverrides(ctx context.Context, db dbExecutor, item objectRecord, config relationshipAggregation, overrides map[string]json.RawMessage) (float64, error) {
	raw, overridden := overrides[config.RelationshipFieldID]
	if !overridden {
		return a.relationshipAggregate(ctx, db, item, config)
	}
	ids, err := mergeOverrideIDs(raw)
	if err != nil {
		return 0, err
	}
	count, result := 0, 0.0
	for _, id := range ids {
		var rawValue []byte
		err := db.QueryRowContext(ctx, `SELECT COALESCE(v.value_json,f.default_json) FROM objects o JOIN object_types t ON t.id=o.object_type_id LEFT JOIN field_definitions f ON f.id=? AND f.object_type_id=o.object_type_id AND f.archived=0 LEFT JOIN field_values v ON v.object_id=o.id AND v.field_id=f.id WHERE o.id=? AND o.game_id=? AND o.archived=0 AND t.archived=0`, config.FieldID, id, item.GameID).Scan(&rawValue)
		if err == sql.ErrNoRows {
			continue
		}
		if err != nil {
			return 0, err
		}
		if config.Operation == "count" {
			count++
			continue
		}
		value, err := numericValue(rawValue)
		if err != nil {
			continue
		}
		if count == 0 && (config.Operation == "min" || config.Operation == "max") {
			result = value
		} else {
			switch config.Operation {
			case "sum", "avg":
				result += value
			case "min":
				result = math.Min(result, value)
			case "max":
				result = math.Max(result, value)
			}
		}
		count++
	}
	if config.Operation == "count" {
		return float64(count), nil
	}
	if (config.Operation == "avg" || config.Operation == "min" || config.Operation == "max") && count == 0 {
		return 0, fmt.Errorf("no linked numeric values")
	}
	if config.Operation == "avg" {
		result /= float64(count)
	}
	if math.IsNaN(result) || math.IsInf(result, 0) {
		return 0, fmt.Errorf("aggregation result is not finite")
	}
	return result, nil
}
