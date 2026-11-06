package httpapi

import (
	"encoding/json"
	"net/http"
)

type comparisonRequestItem struct {
	ObjectID string `json:"object_id"`
	PresetID string `json:"preset_id"`
}

type comparisonSubject struct {
	Object         objectSummary              `json:"object"`
	Fields         []fieldDefinition          `json:"fields"`
	Values         map[string]json.RawMessage `json:"values"`
	FormulaErrors  map[string]string          `json:"formula_errors,omitempty"`
	ReferenceNames map[string][]string        `json:"reference_names,omitempty"`
	PresetID       string                     `json:"preset_id,omitempty"`
	PresetName     string                     `json:"preset_name,omitempty"`
}

func (a *API) compareObjects(w http.ResponseWriter, r *http.Request) {
	gameID := r.PathValue("gameID")
	if _, err := a.loadGame(r, gameID, false); err != nil {
		notFoundOrError(w, err, "Game")
		return
	}
	var input struct {
		Items []comparisonRequestItem `json:"items"`
	}
	if !decodeJSON(w, r, &input) {
		return
	}
	if len(input.Items) < 2 || len(input.Items) > 4 {
		writeError(w, http.StatusBadRequest, "invalid_comparison", "Choose between two and four objects or configurations to compare.")
		return
	}
	subjects := make([]comparisonSubject, 0, len(input.Items))
	seen := map[string]bool{}
	typeID := ""
	for _, selection := range input.Items {
		if selection.ObjectID == "" {
			writeError(w, http.StatusBadRequest, "invalid_comparison", "Each comparison item must identify an object.")
			return
		}
		key := selection.ObjectID + ":" + selection.PresetID
		if seen[key] {
			writeError(w, http.StatusBadRequest, "invalid_comparison", "Each object configuration can appear only once.")
			return
		}
		seen[key] = true
		item, err := a.loadObject(r, selection.ObjectID)
		if err != nil || item.GameID != gameID {
			writeError(w, http.StatusNotFound, "not_found", "Every compared object must be active and belong to this game.")
			return
		}
		if typeID == "" {
			typeID = item.ObjectTypeID
		} else if item.ObjectTypeID != typeID {
			writeError(w, http.StatusBadRequest, "incompatible_types", "Compare objects that use the same object type and field schema.")
			return
		}
		subject, err := a.comparisonSubject(r, item, selection.PresetID)
		if err != nil {
			if validation, ok := err.(validationError); ok {
				writeError(w, http.StatusBadRequest, validation.code, validation.message)
			} else {
				writeError(w, http.StatusInternalServerError, "comparison_failed", "Could not prepare this comparison.")
			}
			return
		}
		subjects = append(subjects, subject)
	}
	writeJSON(w, http.StatusOK, map[string]any{"object_type_id": typeID, "subjects": subjects})
}

func (a *API) comparisonSubject(r *http.Request, item objectRecord, presetID string) (comparisonSubject, error) {
	fields, err := loadFieldsFrom(r.Context(), a.db, item.ObjectTypeID, false)
	if err != nil {
		return comparisonSubject{}, err
	}
	values, err := a.objectValues(r.Context(), a.db, item, fields)
	if err != nil {
		return comparisonSubject{}, err
	}
	name := ""
	if presetID != "" {
		var raw string
		err := a.db.QueryRowContext(r.Context(), `SELECT name,values_json FROM object_presets WHERE id=? AND object_id=? AND game_id=?`, presetID, item.ID, item.GameID).Scan(&name, &raw)
		if err != nil {
			return comparisonSubject{}, validationError{code: "invalid_preset", message: "Each selected preset must belong to its comparison object."}
		}
		var overrides map[string]json.RawMessage
		if err := json.Unmarshal([]byte(raw), &overrides); err != nil {
			return comparisonSubject{}, err
		}
		values = mergePresetValues(values, overrides)
		if err := a.calculateFormulasWithOverrides(r.Context(), a.db, item, fields, values, overrides); err != nil {
			return comparisonSubject{}, err
		}
	}
	refs, err := a.comparisonReferenceNames(r, item.GameID, fields, values)
	if err != nil {
		return comparisonSubject{}, err
	}
	return comparisonSubject{Object: item.objectSummary, Fields: fields, Values: values, FormulaErrors: formulaErrors(fields, values), ReferenceNames: refs, PresetID: presetID, PresetName: name}, nil
}

func (a *API) comparisonReferenceNames(r *http.Request, gameID string, fields []fieldDefinition, values map[string]json.RawMessage) (map[string][]string, error) {
	result := map[string][]string{}
	for _, field := range fields {
		if field.FieldType != "reference" && field.FieldType != "references" {
			continue
		}
		var ids []string
		if field.FieldType == "reference" {
			var id string
			if err := json.Unmarshal(values[field.ID], &id); err == nil && id != "" {
				ids = []string{id}
			}
		} else if err := json.Unmarshal(values[field.ID], &ids); err != nil {
			continue
		}
		labels := make([]string, 0, len(ids))
		for _, id := range uniqueSortedStrings(ids) {
			var name string
			err := a.db.QueryRowContext(r.Context(), `SELECT name FROM objects WHERE id=? AND game_id=?`, id, gameID).Scan(&name)
			if err != nil {
				name = "Unavailable object " + id[:minInt(len(id), 8)]
			}
			labels = append(labels, name)
		}
		result[field.ID] = labels
	}
	return result, nil
}

func minInt(a, b int) int {
	if a < b {
		return a
	}
	return b
}
