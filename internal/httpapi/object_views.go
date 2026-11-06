package httpapi

import (
	"context"
	"encoding/json"
	"fmt"
	"strings"
	"time"
)

type objectView struct {
	Search     string         `json:"search"`
	TypeID     string         `json:"type_id"`
	Mode       string         `json:"mode"`
	Sort       string         `json:"sort"`
	Direction  string         `json:"direction"`
	Conditions []objectFilter `json:"conditions"`
}
type objectFilter struct {
	FieldID  string          `json:"field_id"`
	Operator string          `json:"operator"`
	Value    json.RawMessage `json:"value"`
	ValueTo  json.RawMessage `json:"value_to"`
}

func (a *API) validateObjectView(ctx context.Context, gameID string, view *objectView) error {
	if view == nil {
		return nil
	}
	if len(view.Search) > 2000 || len(view.Conditions) > 20 {
		return fmt.Errorf("Use a search up to 2000 characters and no more than 20 filters.")
	}
	if view.Mode != "all" && view.Mode != "any" {
		return fmt.Errorf("Choose ALL or ANY filters.")
	}
	if view.Direction != "asc" && view.Direction != "desc" {
		return fmt.Errorf("Choose ascending or descending order.")
	}
	if view.TypeID != "" {
		var valid int
		if err := a.db.QueryRowContext(ctx, `SELECT COUNT(*) FROM object_types WHERE id=? AND game_id=? AND archived=0`, view.TypeID, gameID).Scan(&valid); err != nil || valid != 1 {
			return fmt.Errorf("Choose an active object type in this game.")
		}
	}
	loadField := func(id string) (fieldDefinition, error) {
		var f fieldDefinition
		var opts []byte
		err := a.db.QueryRowContext(ctx, `SELECT f.field_type,f.options_json FROM field_definitions f JOIN object_types t ON t.id=f.object_type_id WHERE f.id=? AND f.object_type_id=? AND t.game_id=? AND f.archived=0 AND t.archived=0`, id, view.TypeID, gameID).Scan(&f.FieldType, &opts)
		f.Options = opts
		return f, err
	}
	if view.Sort != "name" && view.Sort != "type_name" && view.Sort != "created_at" && view.Sort != "updated_at" {
		f, err := loadField(view.Sort)
		if err != nil || f.FieldType == "reference" || f.FieldType == "references" || f.FieldType == "long_text" {
			return fmt.Errorf("Choose a compatible sort field on the selected type.")
		}
	}
	for _, c := range view.Conditions {
		f, err := loadField(c.FieldID)
		if err != nil {
			return fmt.Errorf("Every filter must use an active field on the selected object type.")
		}
		allowed := ""
		switch f.FieldType {
		case "integer", "decimal", "percentage", "formula":
			allowed = "eq ne gt gte lt lte between"
		case "text", "long_text":
			allowed = "contains eq not_contains starts_with"
		case "select":
			allowed = "eq ne one_of"
		case "boolean":
			allowed = "checked unchecked"
		case "date":
			allowed = "before after on between"
		case "progress":
			allowed = "below above completed not_completed"
		case "reference", "references":
			allowed = "eq ne"
		}
		if c.Operator == "" || !strings.Contains(" "+allowed+" ", " "+c.Operator+" ") {
			return fmt.Errorf("Choose a comparison supported by the filter field.")
		}
		if f.FieldType == "boolean" || c.Operator == "completed" || c.Operator == "not_completed" {
			continue
		}
		switch f.FieldType {
		case "integer", "decimal", "percentage", "formula", "progress":
			n, err := numberFromJSON(c.Value)
			if err != nil {
				return fmt.Errorf("Numeric filters need finite numbers.")
			}
			if c.Operator == "between" {
				upper, err := numberFromJSON(c.ValueTo)
				if err != nil || upper < n {
					return fmt.Errorf("The upper filter bound must be at least the lower bound.")
				}
			}
		case "date":
			var value string
			if json.Unmarshal(c.Value, &value) != nil {
				return fmt.Errorf("Choose a valid filter date.")
			}
			if _, err := time.Parse("2006-01-02", value); err != nil {
				return fmt.Errorf("Choose a valid filter date.")
			}
			if c.Operator == "between" {
				var upper string
				if json.Unmarshal(c.ValueTo, &upper) != nil {
					return fmt.Errorf("Choose an upper date.")
				}
				if _, err := time.Parse("2006-01-02", upper); err != nil || upper < value {
					return fmt.Errorf("Choose an upper date on or after the lower date.")
				}
			}
		case "select":
			var values []json.RawMessage
			if c.Operator == "one_of" {
				if json.Unmarshal(c.Value, &values) != nil || len(values) == 0 {
					return fmt.Errorf("Select at least one filter option.")
				}
			} else {
				values = []json.RawMessage{c.Value}
			}
			for _, value := range values {
				if _, _, empty, err := validateFieldValue(f, value); err != nil || empty {
					return fmt.Errorf("Choose valid filter options.")
				}
			}
		case "reference", "references":
			var id string
			var typeID string
			if json.Unmarshal(c.Value, &id) != nil {
				return fmt.Errorf("Choose a referenced object.")
			}
			if err := a.db.QueryRowContext(ctx, `SELECT o.object_type_id FROM objects o JOIN object_types t ON t.id=o.object_type_id WHERE o.id=? AND o.game_id=? AND o.archived=0 AND t.archived=0`, id, gameID).Scan(&typeID); err != nil {
				return fmt.Errorf("Choose an active referenced object in this game.")
			}
			var options struct {
				AllowedTypeID string `json:"allowed_type_id"`
			}
			_ = json.Unmarshal(f.Options, &options)
			if options.AllowedTypeID != "" && options.AllowedTypeID != typeID {
				return fmt.Errorf("The reference filter must match the field's allowed type.")
			}
		default:
			var value string
			if json.Unmarshal(c.Value, &value) != nil || len(value) > 50000 {
				return fmt.Errorf("Text filters need a text value.")
			}
		}
	}
	return nil
}
