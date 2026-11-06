package httpapi

import "testing"

func TestObjectPresetsKeepBaseValuesAndRecalculateFormulas(t *testing.T) {
	f := newImprovementFixture(t)
	formulaID := f.create(t, "/api/object-types/"+f.typ+"/fields", map[string]any{"name": "Doubled", "field_type": "formula", "options": map[string]any{"expression": "@{" + f.number + "} * 2"}})
	created := decodeMap(t, f.u.expect("POST", "/api/objects/"+f.object+"/presets", map[string]any{"name": "High", "values": map[string]any{f.number: 30}}, 201))
	presetID := mapString(created, "id")
	listed := decodeMap(t, f.u.expect("GET", "/api/objects/"+f.object+"/presets", nil, 200))
	items := listed["items"].([]any)
	preset := items[0].(map[string]any)
	effective := preset["effective_values"].(map[string]any)
	if effective[f.number] != float64(30) || effective[formulaID] != float64(60) {
		t.Fatalf("effective preset values/formula=%#v", effective)
	}
	if listed["canonical_values"].(map[string]any)[f.number] != float64(10) {
		t.Fatalf("canonical value changed: %#v", listed["canonical_values"])
	}
	f.u.expect("PATCH", "/api/object-presets/"+presetID, map[string]any{"name": "High renamed", "values": map[string]any{f.number: 40}}, 200)
	f.u.expect("DELETE", "/api/object-presets/"+presetID, nil, 204)
	var baseValue string
	if err := f.e.db.QueryRow(`SELECT value_json FROM field_values WHERE object_id=? AND field_id=?`, f.object, f.number).Scan(&baseValue); err != nil || baseValue != "10" {
		t.Fatalf("canonical database value changed: %q err=%v", baseValue, err)
	}
	history := decodeMap(t, f.u.expect("GET", "/api/objects/"+f.object+"/history?limit=20&offset=0", nil, 200))
	changes := history["items"].([]any)
	if len(changes) < 3 {
		t.Fatalf("preset history missing create, update, delete events: %#v", history)
	}
	for _, raw := range changes[:3] {
		entry := raw.(map[string]any)
		found := false
		for _, changeRaw := range entry["changes"].([]any) {
			if changeRaw.(map[string]any)["field_type"] == "preset" {
				found = true
				break
			}
		}
		if !found {
			t.Fatalf("preset audit was confused with canonical changes: %#v", entry)
		}
	}
}

func TestPresetRelationshipOverridesAffectFormulaAggregateOnlyInEffectiveView(t *testing.T) {
	f := newImprovementFixture(t)
	relation := f.create(t, "/api/object-types/"+f.typ+"/fields", map[string]any{"name": "Related", "field_type": "references", "options": map[string]any{"allowed_type_id": f.typ}})
	formulaID := f.create(t, "/api/object-types/"+f.typ+"/fields", map[string]any{"name": "Related sum", "field_type": "formula", "options": map[string]any{"expression": "@{related} + @{" + f.number + "}", "aggregation": map[string]any{"relationship_field_id": relation, "field_id": f.number, "operation": "sum"}}})
	child := f.create(t, "/api/games/"+f.game+"/objects", map[string]any{"name": "Child", "object_type_id": f.typ, "values": map[string]any{f.number: 5}})
	presetID := mapString(decodeMap(t, f.u.expect("POST", "/api/objects/"+f.object+"/presets", map[string]any{"name": "Linked view", "values": map[string]any{relation: []string{child}}}, 201)), "id")
	listed := decodeMap(t, f.u.expect("GET", "/api/objects/"+f.object+"/presets", nil, 200))
	preset := listed["items"].([]any)[0].(map[string]any)
	if preset["id"] != presetID || preset["effective_values"].(map[string]any)[formulaID] != float64(15) {
		t.Fatalf("relationship preset formula not evaluated from override: %#v", preset)
	}
	var canonicalLinks int
	if err := f.e.db.QueryRow(`SELECT COUNT(*) FROM object_relationships WHERE object_id=? AND field_id=?`, f.object, relation).Scan(&canonicalLinks); err != nil || canonicalLinks != 0 {
		t.Fatalf("preset relationship changed canonical object: links=%d err=%v", canonicalLinks, err)
	}
	f.u.expect("PATCH", "/api/object-presets/"+presetID, map[string]any{"values": map[string]any{formulaID: 999}}, 400)
}

func TestPresetOwnershipAndReferenceValidation(t *testing.T) {
	f := newImprovementFixture(t)
	relation := f.create(t, "/api/object-types/"+f.typ+"/fields", map[string]any{"name": "Related", "field_type": "reference", "options": map[string]any{"allowed_type_id": f.typ}})
	foreign := newTestEnvironment(t).client()
	foreign.register("preset_foreign")
	f.u.expect("POST", "/api/objects/"+f.object+"/presets", map[string]any{"name": "Bad reference", "values": map[string]any{relation: "outside"}}, 400)
	foreign.expect("GET", "/api/objects/"+f.object+"/presets", nil, 404)
	presetID := mapString(decodeMap(t, f.u.expect("POST", "/api/objects/"+f.object+"/presets", map[string]any{"name": "Valid"}, 201)), "id")
	foreign.expect("PATCH", "/api/object-presets/"+presetID, map[string]any{"name": "Intrusion"}, 404)
}
