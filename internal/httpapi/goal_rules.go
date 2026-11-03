package httpapi

import (
	"context"
	"database/sql"
	"encoding/json"
	"fmt"
	"math"
	"strings"
)

func (a *API) validateGoalRule(ctx context.Context, gameID string, rule goalRule) error {
	if rule.Mode != "all" && rule.Mode != "any" {
		return fmt.Errorf("Choose all or any conditions.")
	}
	if len(rule.Conditions) == 0 || len(rule.Conditions) > 20 {
		return fmt.Errorf("Use between 1 and 20 goal conditions.")
	}
	for _, c := range rule.Conditions {
		var f fieldDefinition
		var options []byte
		err := a.db.QueryRowContext(ctx, `SELECT f.field_type,f.options_json FROM field_definitions f JOIN objects o ON o.object_type_id=f.object_type_id JOIN object_types t ON t.id=o.object_type_id WHERE o.id=? AND f.id=? AND o.game_id=? AND o.archived=0 AND f.archived=0 AND t.archived=0`, c.ObjectID, c.FieldID, gameID).Scan(&f.FieldType, &options)
		if err != nil {
			return fmt.Errorf("Every condition must use an active field on its object in this game.")
		}
		f.Options = options
		switch f.FieldType {
		case "integer", "decimal", "percentage", "progress", "formula":
			if _, err := numberFromJSON(c.Value); err != nil {
				return fmt.Errorf("Numeric conditions need a finite numeric target.")
			}
			if c.Operator != "eq" && c.Operator != "ne" && c.Operator != "gte" && c.Operator != "lte" && c.Operator != "gt" && c.Operator != "lt" {
				return fmt.Errorf("Choose a numeric comparison.")
			}
		case "text", "long_text", "select", "date", "boolean":
			if _, _, empty, err := validateFieldValue(f, c.Value); err != nil || empty {
				return fmt.Errorf("Enter a valid target for each condition's field type.")
			}
			if c.Operator != "eq" && c.Operator != "ne" && !(f.FieldType == "date" && (c.Operator == "gte" || c.Operator == "lte" || c.Operator == "gt" || c.Operator == "lt")) && !((f.FieldType == "text" || f.FieldType == "long_text") && c.Operator == "contains") {
				return fmt.Errorf("Choose a comparison supported by this field type.")
			}
		default:
			return fmt.Errorf("Relationship fields cannot be goal conditions.")
		}
	}
	return nil
}

func (a *API) populateRuleProgress(ctx context.Context, item *goal, rule goalRule) error {
	done := 0
	for _, condition := range rule.Conditions {
		var record objectRecord
		err := a.db.QueryRowContext(ctx, `SELECT o.id,o.game_id,o.object_type_id FROM objects o JOIN object_types t ON t.id=o.object_type_id WHERE o.id=? AND o.game_id=? AND o.archived=0 AND t.archived=0`, condition.ObjectID, item.GameID).Scan(&record.ID, &record.GameID, &record.ObjectTypeID)
		if err == sql.ErrNoRows {
			continue
		}
		if err != nil {
			return err
		}
		fields, err := a.loadFields(ctx, record.ObjectTypeID, false)
		if err != nil {
			return err
		}
		values, err := a.objectValues(ctx, a.db, record, fields)
		if err != nil {
			return err
		}
		for _, field := range fields {
			if field.ID == condition.FieldID && conditionMatches(values[field.ID], condition, field.FieldType) {
				done++
				break
			}
		}
	}
	item.Current = float64(done)
	item.Completed = len(rule.Conditions) > 0 && (done == len(rule.Conditions) || rule.Mode == "any" && done > 0)
	if len(rule.Conditions) > 0 {
		item.Progress = float64(done) / float64(len(rule.Conditions)) * 100
	}
	if rule.Mode == "any" && item.Completed {
		item.Progress = 100
	}
	return nil
}

func conditionMatches(raw json.RawMessage, c goalCondition, fieldType string) bool {
	if len(raw) == 0 || string(raw) == "null" {
		return false
	}
	comparison := 0
	switch fieldType {
	case "integer", "decimal", "percentage", "progress", "formula":
		value, err := numericValue(raw)
		target, targetErr := numberFromJSON(c.Value)
		if err != nil || targetErr != nil {
			return false
		}
		if value < target {
			comparison = -1
		} else if value > target {
			comparison = 1
		}
	default:
		var value, target any
		if json.Unmarshal(raw, &value) != nil || json.Unmarshal(c.Value, &target) != nil {
			return false
		}
		if c.Operator == "contains" {
			v, ok := value.(string)
			t, tok := target.(string)
			return ok && tok && strings.Contains(v, t)
		}
		comparison = strings.Compare(fmt.Sprint(value), fmt.Sprint(target))
	}
	switch c.Operator {
	case "eq":
		return comparison == 0
	case "ne":
		return comparison != 0
	case "gt":
		return comparison > 0
	case "gte":
		return comparison >= 0
	case "lt":
		return comparison < 0
	case "lte":
		return comparison <= 0
	}
	return false
}

func numericValue(raw json.RawMessage) (float64, error) {
	if len(raw) > 0 && raw[0] == '{' {
		var p struct {
			Current json.RawMessage `json:"current"`
		}
		if err := json.Unmarshal(raw, &p); err != nil {
			return 0, err
		}
		raw = p.Current
	}
	n, err := numberFromJSON(raw)
	if err != nil || math.IsNaN(n) || math.IsInf(n, 0) {
		return 0, fmt.Errorf("missing numeric value")
	}
	return n, nil
}
