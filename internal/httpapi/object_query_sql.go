package httpapi

import (
	"context"
	"encoding/json"
	"fmt"
	"strings"
)

func (a *API) objectQueryFieldTypes(ctx context.Context, gameID string, view objectView) (map[string]string, error) {
	unique := make(map[string]struct{}, len(view.Conditions)+1)
	if view.Sort != "name" && view.Sort != "type_name" && view.Sort != "created_at" && view.Sort != "updated_at" {
		unique[view.Sort] = struct{}{}
	}
	for _, condition := range view.Conditions {
		unique[condition.FieldID] = struct{}{}
	}
	fieldTypes := make(map[string]string, len(unique))
	if len(unique) == 0 {
		return fieldTypes, nil
	}
	ids := make([]string, 0, len(unique))
	for id := range unique {
		ids = append(ids, id)
	}
	pl := strings.TrimSuffix(strings.Repeat("?,", len(ids)), ",")
	args := []any{view.TypeID, gameID}
	for _, id := range ids {
		args = append(args, id)
	}
	rows, err := a.db.QueryContext(ctx, `SELECT f.id,f.field_type FROM field_definitions f JOIN object_types t ON t.id=f.object_type_id WHERE f.object_type_id=? AND t.game_id=? AND f.archived=0 AND t.archived=0 AND f.id IN (`+pl+`)`, args...)
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	for rows.Next() {
		var id, fieldType string
		if err := rows.Scan(&id, &fieldType); err != nil {
			return nil, err
		}
		fieldTypes[id] = fieldType
	}
	if err := rows.Err(); err != nil {
		return nil, err
	}
	if len(fieldTypes) != len(unique) {
		return nil, fmt.Errorf("object query field is unavailable")
	}
	return fieldTypes, nil
}

func objectSQLFilters(view objectView, fieldTypes map[string]string) (string, []any, bool) {
	if len(view.Conditions) == 0 {
		return "", nil, true
	}
	separator := " AND "
	if view.Mode == "any" {
		separator = " OR "
	}
	predicates := make([]string, 0, len(view.Conditions))
	args := make([]any, 0, len(view.Conditions)*3)
	for _, condition := range view.Conditions {
		predicate, conditionArgs, ok := objectSQLFilter(condition, fieldTypes[condition.FieldID])
		if !ok {
			return "", nil, false
		}
		predicates = append(predicates, `EXISTS (SELECT 1 FROM field_definitions fd LEFT JOIN field_values fv ON fv.object_id=o.id AND fv.field_id=fd.id WHERE fd.id=? AND fd.object_type_id=o.object_type_id AND fd.archived=0 AND (`+predicate+`))`)
		args = append(args, condition.FieldID)
		args = append(args, conditionArgs...)
	}
	return "(" + strings.Join(predicates, separator) + ")", args, true
}

func objectSQLFilter(condition objectFilter, fieldType string) (string, []any, bool) {
	raw := `CASE WHEN fv.value_json IS NOT NULL THEN fv.value_json ELSE fd.default_json END`
	kind := `json_type(` + raw + `,'$')`
	value := `json_extract(` + raw + `,'$')`
	switch fieldType {
	case "integer", "decimal", "percentage":
		operator := map[string]string{"eq": "=", "ne": "<>", "gt": ">", "gte": ">=", "lt": "<", "lte": "<=", "between": "BETWEEN"}[condition.Operator]
		bound, err := numberFromJSON(condition.Value)
		if err != nil || operator == "" {
			return "", nil, false
		}
		number := "CAST(" + value + " AS REAL)"
		typed := kind + ` IN ('integer','real')`
		if condition.Operator == "between" {
			upper, err := numberFromJSON(condition.ValueTo)
			if err != nil {
				return "", nil, false
			}
			return typed + " AND " + number + " BETWEEN ? AND ?", []any{bound, upper}, true
		}
		return typed + " AND " + number + " " + operator + " ?", []any{bound}, true
	case "text", "long_text":
		if condition.Operator != "eq" && condition.Operator != "contains" && condition.Operator != "not_contains" && condition.Operator != "starts_with" {
			return "", nil, false
		}
		var target string
		if json.Unmarshal(condition.Value, &target) != nil {
			return "", nil, false
		}
		textValue := "lower(CAST(" + value + " AS TEXT))"
		typed := kind + `='text'`
		switch condition.Operator {
		case "eq":
			return typed + " AND " + textValue + "=?", []any{strings.ToLower(target)}, true
		case "contains":
			return typed + " AND instr(" + textValue + ",lower(?))>0", []any{target}, true
		case "not_contains":
			return typed + " AND instr(" + textValue + ",lower(?))=0", []any{target}, true
		default:
			return typed + " AND instr(" + textValue + ",lower(?))=1", []any{target}, true
		}
	case "select":
		if condition.Operator == "one_of" {
			var choices []string
			if json.Unmarshal(condition.Value, &choices) != nil || len(choices) == 0 {
				return "", nil, false
			}
			placeholders := strings.TrimSuffix(strings.Repeat("?,", len(choices)), ",")
			args := make([]any, len(choices))
			for i := range choices {
				args[i] = choices[i]
			}
			return kind + `='text' AND ` + value + " IN (" + placeholders + ")", args, true
		}
		var target string
		if json.Unmarshal(condition.Value, &target) != nil {
			return "", nil, false
		}
		operator := map[string]string{"eq": "=", "ne": "<>"}[condition.Operator]
		if operator == "" {
			return "", nil, false
		}
		return kind + `='text' AND ` + value + " " + operator + " ?", []any{target}, true
	case "boolean":
		if condition.Operator != "checked" && condition.Operator != "unchecked" {
			return "", nil, false
		}
		want := "true"
		if condition.Operator == "unchecked" {
			want = "false"
		}
		return kind + "=?", []any{want}, true
	case "date":
		var target string
		if json.Unmarshal(condition.Value, &target) != nil {
			return "", nil, false
		}
		op := map[string]string{"before": "<", "after": ">", "on": "=", "between": "BETWEEN"}[condition.Operator]
		if op == "" {
			return "", nil, false
		}
		textValue := "CAST(" + value + " AS TEXT)"
		if condition.Operator == "between" {
			var upper string
			if json.Unmarshal(condition.ValueTo, &upper) != nil {
				return "", nil, false
			}
			return kind + `='text' AND ` + textValue + " BETWEEN ? AND ?", []any{target, upper}, true
		}
		return kind + `='text' AND ` + textValue + " " + op + " ?", []any{target}, true
	case "progress":
		if condition.Operator != "below" && condition.Operator != "above" && condition.Operator != "completed" && condition.Operator != "not_completed" {
			return "", nil, false
		}
		current := "CAST(json_extract(" + raw + ",'$.current') AS REAL)"
		maximum := "CAST(json_extract(" + raw + ",'$.max') AS REAL)"
		typed := kind + `='object' AND json_type(` + raw + `,'$.current') IN ('integer','real') AND json_type(` + raw + `,'$.max') IN ('integer','real') AND ` + maximum + ">0"
		percent := "(" + current + "/" + maximum + "*100)"
		if condition.Operator == "completed" {
			return typed + " AND " + percent + ">=100", nil, true
		}
		if condition.Operator == "not_completed" {
			return typed + " AND " + percent + "<100", nil, true
		}
		bound, err := numberFromJSON(condition.Value)
		if err != nil {
			return "", nil, false
		}
		op := "<"
		if condition.Operator == "above" {
			op = ">"
		}
		return typed + " AND " + percent + " " + op + " ?", []any{bound}, true
	default:
		// Formula evaluation and relationship semantics stay in the existing Go
		// evaluator; they are never translated into SQL.
		return "", nil, false
	}
}

func objectSQLSort(fieldType string) (string, bool) {
	raw := `CASE WHEN sv.value_json IS NOT NULL THEN sv.value_json ELSE sf.default_json END`
	kind := `json_type(` + raw + `,'$')`
	value := `json_extract(` + raw + `,'$')`
	switch fieldType {
	case "integer", "decimal", "percentage":
		return `CASE WHEN ` + kind + ` IN ('integer','real') THEN CAST(` + value + ` AS REAL) END`, true
	case "boolean":
		return `CASE WHEN ` + kind + ` IN ('true','false') THEN ` + value + ` END`, true
	case "select", "text", "date":
		return `CASE WHEN ` + kind + `='text' THEN CAST(` + value + ` AS TEXT) END`, true
	case "progress":
		current := "CAST(json_extract(" + raw + ",'$.current') AS REAL)"
		maximum := "CAST(json_extract(" + raw + ",'$.max') AS REAL)"
		valid := kind + `='object' AND json_type(` + raw + `,'$.current') IN ('integer','real') AND json_type(` + raw + `,'$.max') IN ('integer','real') AND ` + maximum + ">0"
		return "CASE WHEN " + valid + " THEN " + current + "/" + maximum + " END", true
	default:
		return "", false
	}
}

func customObjectOrder(fieldType, direction string) (string, bool) {
	expression, ok := objectSQLSort(fieldType)
	if !ok {
		return "", false
	}
	if fieldType == "text" || fieldType == "date" || fieldType == "select" {
		expression += " COLLATE NOCASE"
	}
	return fmt.Sprintf("CASE WHEN (%s) IS NULL THEN 1 ELSE 0 END ASC,%s %s,o.id ASC", expression, expression, strings.ToUpper(direction)), true
}
