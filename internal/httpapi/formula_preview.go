package httpapi

import (
	"encoding/json"
	"fmt"
	"net/http"

	"gametracker/internal/formula"
)

// previewFormula validates and evaluates temporary inputs without writing object data.
func (a *API) previewFormula(w http.ResponseWriter, r *http.Request) {
	typ, err := a.loadObjectType(r, r.PathValue("typeID"))
	if err != nil {
		notFoundOrError(w, err, "Object type")
		return
	}
	if typ.Archived {
		writeError(w, 400, "invalid_type", "Choose an active object type.")
		return
	}
	var input struct {
		FieldID  string                     `json:"field_id"`
		ObjectID string                     `json:"object_id"`
		Options  json.RawMessage            `json:"options"`
		Values   map[string]json.RawMessage `json:"values"`
		Related  *float64                   `json:"related"`
	}
	if !decodeJSON(w, r, &input) {
		return
	}
	fields, err := a.loadFields(r.Context(), typ.ID, false)
	if err != nil {
		writeError(w, 500, "database_error", "Could not load fields.")
		return
	}
	if input.FieldID == "" {
		input.FieldID = "preview"
	} else {
		valid := false
		for _, f := range fields {
			if f.ID == input.FieldID && f.FieldType == "formula" {
				valid = true
			}
		}
		if !valid {
			writeError(w, 400, "invalid_field", "Choose an active formula on this object type.")
			return
		}
	}
	if err = a.validateFieldOptionsForPreview(r, typ.ID, input.FieldID, input.Options); err != nil {
		writeError(w, 400, "invalid_formula", err.Error())
		return
	}
	values := map[string]json.RawMessage{}
	item := objectRecord{GameID: typ.GameID}
	if input.ObjectID != "" {
		item, err = a.loadObject(r, input.ObjectID)
		if err != nil || item.ObjectTypeID != typ.ID {
			writeError(w, 400, "invalid_object", "Choose an active object of this type.")
			return
		}
		values, err = a.objectValues(r.Context(), a.db, item, fields)
		if err != nil {
			writeError(w, 500, "database_error", "Could not read example object.")
			return
		}
	}
	fieldMap := map[string]fieldDefinition{}
	for _, f := range fields {
		fieldMap[f.ID] = f
		// Calculated values must be recomputed with the candidate formula and inputs.
		if f.FieldType == "formula" {
			delete(values, f.ID)
		}
		if _, ok := values[f.ID]; !ok && f.FieldType != "formula" && len(f.DefaultValue) > 0 {
			values[f.ID] = f.DefaultValue
		}
	}
	for id, raw := range input.Values {
		f, ok := fieldMap[id]
		if !ok || (f.FieldType != "integer" && f.FieldType != "decimal" && f.FieldType != "percentage" && f.FieldType != "progress") {
			writeError(w, 400, "invalid_values", "Example values must use numeric inputs of this type.")
			return
		}
		if _, _, _, err := validateFieldValue(f, raw); err != nil {
			writeError(w, 400, "invalid_values", err.Error())
			return
		}
		values[id] = raw
	}
	fieldMap[input.FieldID] = fieldDefinition{ID: input.FieldID, FieldType: "formula", Options: input.Options}
	visiting := map[string]bool{}
	memo := map[string]float64{}
	var resolve func(string) (float64, error)
	resolve = func(id string) (float64, error) {
		if n, ok := memo[id]; ok {
			return n, nil
		}
		if visiting[id] || len(visiting) > 128 {
			return 0, fmt.Errorf("circular or excessive formula dependencies")
		}
		f, ok := fieldMap[id]
		if !ok {
			return 0, fmt.Errorf("referenced field is missing or archived")
		}
		if f.FieldType != "formula" {
			n, e := numericValue(values[id])
			if e != nil {
				return 0, fmt.Errorf("%s has no numeric value", f.Name)
			}
			return n, nil
		}
		visiting[id] = true
		defer delete(visiting, id)
		var cfg struct {
			Expression  string                   `json:"expression"`
			Aggregation *relationshipAggregation `json:"aggregation"`
		}
		if err := json.Unmarshal(f.Options, &cfg); err != nil {
			return 0, err
		}
		inputs := map[string]float64{}
		for _, ref := range formula.References(cfg.Expression) {
			var n float64
			var err error
			if ref == "related" && cfg.Aggregation != nil {
				if input.ObjectID != "" {
					n, err = a.relationshipAggregate(r.Context(), a.db, item, *cfg.Aggregation)
				} else if input.Related != nil {
					n = *input.Related
				} else {
					err = fmt.Errorf("enter a temporary relationship result or choose an object")
				}
			} else {
				n, err = resolve(ref)
			}
			if err != nil {
				return 0, err
			}
			inputs[ref] = n
		}
		n, err := formula.Evaluate(cfg.Expression, inputs)
		if err == nil {
			memo[id] = n
		}
		return n, err
	}
	result, err := resolve(input.FieldID)
	if err != nil {
		writeJSON(w, 200, map[string]any{"valid": true, "error": err.Error()})
		return
	}
	writeJSON(w, 200, map[string]any{"valid": true, "result": result})
}

func (a *API) validateFieldOptionsForPreview(r *http.Request, typeID, fieldID string, options json.RawMessage) error {
	if err := validateFieldOptions("formula", options); err != nil {
		return err
	}
	return a.validateFormulaSchema(r.Context(), typeID, fieldID, options)
}
