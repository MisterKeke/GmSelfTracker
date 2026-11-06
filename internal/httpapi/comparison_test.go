package httpapi

import (
	"encoding/json"
	"net/http"
	"testing"
)

func TestObjectComparisonSameTypeDifferencesAndPresetValues(t *testing.T) {
	f := newImprovementFixture(t)
	second := f.create(t, "/api/games/"+f.game+"/objects", map[string]any{"object_type_id": f.typ, "name": "Second", "values": map[string]any{f.number: 12}})
	reference := f.create(t, "/api/object-types/"+f.typ+"/fields", map[string]any{"name": "Related object", "field_type": "reference", "options": map[string]any{"allowed_type_id": f.typ}})
	f.u.expect("PATCH", "/api/objects/"+f.object+"/values", map[string]any{"values": map[string]any{reference: second}}, http.StatusOK)
	preset := decodeMap(t, f.u.expect("POST", "/api/objects/"+f.object+"/presets", map[string]any{"name": "Alternate", "values": map[string]any{f.number: 15}}, http.StatusCreated))
	path := "/api/games/" + f.game + "/comparison"
	var result struct {
		Subjects []comparisonSubject `json:"subjects"`
	}
	body := map[string]any{"items": []any{map[string]any{"object_id": f.object}, map[string]any{"object_id": second}, map[string]any{"object_id": f.object, "preset_id": preset["id"]}}}
	if err := json.Unmarshal(f.u.expect("POST", path, body, http.StatusOK), &result); err != nil {
		t.Fatal(err)
	}
	if len(result.Subjects) != 3 || result.Subjects[0].Values[f.number] == nil || result.Subjects[2].PresetName != "Alternate" {
		t.Fatalf("comparison subjects were not loaded: %#v", result.Subjects)
	}
	var base, alternate float64
	if err := json.Unmarshal(result.Subjects[0].Values[f.number], &base); err != nil {
		t.Fatal(err)
	}
	if err := json.Unmarshal(result.Subjects[2].Values[f.number], &alternate); err != nil {
		t.Fatal(err)
	}
	if base != 10 || alternate != 15 {
		t.Fatalf("comparison should use the base and effective preset values: %v and %v", base, alternate)
	}
	if labels := result.Subjects[0].ReferenceNames[reference]; len(labels) != 1 || labels[0] != "Second" {
		t.Fatalf("comparison should resolve reference labels within the game: %#v", result.Subjects[0].ReferenceNames)
	}
}

func TestObjectComparisonRejectsInvalidTypesPresetsAndOwners(t *testing.T) {
	f := newImprovementFixture(t)
	otherType := f.create(t, "/api/games/"+f.game+"/object-types", map[string]any{"name": "Other type"})
	otherObject := f.create(t, "/api/games/"+f.game+"/objects", map[string]any{"object_type_id": otherType, "name": "Other"})
	path := "/api/games/" + f.game + "/comparison"
	f.u.expect("POST", path, map[string]any{"items": []any{map[string]any{"object_id": f.object}}}, http.StatusBadRequest)
	f.u.expect("POST", path, map[string]any{"items": []any{map[string]any{"object_id": f.object}, map[string]any{"object_id": otherObject}}}, http.StatusBadRequest)
	f.u.expect("POST", path, map[string]any{"items": []any{map[string]any{"object_id": f.object, "preset_id": "foreign-preset"}, map[string]any{"object_id": f.object}}}, http.StatusBadRequest)
	otherGame := f.create(t, "/api/games", map[string]any{"name": "Other game"})
	otherGameObject := f.create(t, "/api/games/"+otherGame+"/objects", map[string]any{"object_type_id": f.create(t, "/api/games/"+otherGame+"/object-types", map[string]any{"name": "Entry"}), "name": "Foreign"})
	f.u.expect("POST", path, map[string]any{"items": []any{map[string]any{"object_id": f.object}, map[string]any{"object_id": otherGameObject}}}, http.StatusNotFound)
	outsider := f.e.client()
	outsider.register("comparison_outsider")
	outsider.expect("POST", path, map[string]any{"items": []any{map[string]any{"object_id": f.object}, map[string]any{"object_id": f.object, "preset_id": "foreign-preset"}}}, http.StatusNotFound)
}
