package httpapi

import (
	"encoding/json"
	"strings"
	"testing"
)

func TestFieldMigrationConversionRules(t *testing.T) {
	tests := []struct {
		name, from, target string
		options            json.RawMessage
		policy             map[string]json.RawMessage
		value              string
		want               string
		wantError          string
	}{
		{name: "text to integer", from: "text", target: "integer", value: `"90"`, want: `90`},
		{name: "decimal integral to integer", from: "decimal", target: "integer", value: `90.0`, want: `90`},
		{name: "fraction to integer blocked", from: "decimal", target: "integer", value: `90.5`, wantError: "mathematically integral"},
		{name: "integer to decimal", from: "integer", target: "decimal", value: `90`, want: `90`},
		{name: "percentage preserves meaning", from: "percentage", target: "decimal", value: `12.500`, want: `12.500`},
		{name: "out of range percentage blocked", from: "integer", target: "percentage", value: `120`, wantError: "explicit clamp strategy"},
		{name: "explicit percentage clamp", from: "integer", target: "percentage", policy: map[string]json.RawMessage{"percentage_strategy": json.RawMessage(`"clamp"`)}, value: `120`, want: `100`},
		{name: "number to exact text", from: "decimal", target: "text", value: `1.2300e2`, want: `"123"`},
		{name: "negative number to exact text", from: "decimal", target: "text", value: `-0.0500`, want: `"-0.05"`},
		{name: "text to checkbox", from: "text", target: "boolean", value: `"false"`, want: `false`},
		{name: "ambiguous checkbox text blocked", from: "text", target: "boolean", value: `"yes"`, wantError: "exactly true or false"},
		{name: "text maps to select choice", from: "text", target: "select", options: json.RawMessage(`{"choices":["Complete"]}`), policy: map[string]json.RawMessage{"select_replacements": json.RawMessage(`{"Done":"Complete"}`)}, value: `"Done"`, want: `"Complete"`},
		{name: "invalid select choice blocked", from: "text", target: "select", options: json.RawMessage(`{"choices":["Complete"]}`), value: `"Unknown"`, wantError: "does not match"},
		{name: "canonical date", from: "text", target: "date", value: `"2026-11-05"`, want: `"2026-11-05"`},
		{name: "noncanonical date blocked", from: "text", target: "date", value: `"11/05/2026"`, wantError: "canonical ISO date"},
		{name: "progress explicit current", from: "progress", target: "decimal", policy: map[string]json.RawMessage{"progress_value": json.RawMessage(`"current"`)}, value: `{"current":7,"max":12}`, want: `7`},
		{name: "progress explicit percent", from: "progress", target: "percentage", policy: map[string]json.RawMessage{"progress_value": json.RawMessage(`"percentage"`)}, value: `{"current":3,"max":12}`, want: `25`},
		{name: "progress percentage out of range", from: "progress", target: "percentage", policy: map[string]json.RawMessage{"progress_value": json.RawMessage(`"percentage"`)}, value: `{"current":13,"max":12}`, wantError: "explicit clamp strategy"},
		{name: "number to progress needs maximum", from: "integer", target: "progress", policy: map[string]json.RawMessage{"maximum": json.RawMessage(`100`)}, value: `25`, want: `{"current":25,"max":100}`},
		{name: "progress maximum must be positive", from: "integer", target: "progress", policy: map[string]json.RawMessage{"maximum": json.RawMessage(`0`)}, value: `25`, wantError: "greater than zero"},
	}
	for _, test := range tests {
		t.Run(test.name, func(t *testing.T) {
			target := fieldDefinition{FieldType: test.target, Options: test.options}
			got, err := convertFieldTypeValue(test.from, target, test.policy, []byte(test.value))
			if test.wantError != "" {
				if err == nil || !strings.Contains(err.Error(), test.wantError) {
					t.Fatalf("error=%v, want substring %q", err, test.wantError)
				}
				return
			}
			if err != nil {
				t.Fatal(err)
			}
			if string(got) != test.want {
				t.Fatalf("converted=%s, want %s", got, test.want)
			}
		})
	}
}

func TestCanonicalDecimalTextIsExact(t *testing.T) {
	for _, value := range []string{"0.1", "1.2300e2", "-0.0500", "100000000000000000000.25"} {
		got, ok := canonicalDecimalText(value)
		if !ok {
			t.Fatalf("canonicalDecimalText(%q) failed", value)
		}
		original, ok := decimalRat(value)
		if !ok {
			t.Fatalf("decimalRat(%q) failed", value)
		}
		converted, ok := decimalRat(got)
		if !ok || converted.Cmp(original) != 0 {
			t.Fatalf("%q became %q, exact value changed", value, got)
		}
	}
}

func TestFieldTypeMigrationPreviewApplyAndStaleReview(t *testing.T) {
	f := newImprovementFixture(t)
	textField := f.create(t, "/api/object-types/"+f.typ+"/fields", map[string]any{"name": "Rank text", "field_type": "text"})
	validObject := f.create(t, "/api/games/"+f.game+"/objects", map[string]any{"name": "Valid rank", "object_type_id": f.typ, "values": map[string]any{textField: "90.0"}})
	invalidObject := f.create(t, "/api/games/"+f.game+"/objects", map[string]any{"name": "Invalid rank", "object_type_id": f.typ, "values": map[string]any{textField: "unknown"}})
	archivedObject := f.create(t, "/api/games/"+f.game+"/objects", map[string]any{"name": "Archived rank", "object_type_id": f.typ, "values": map[string]any{textField: "91"}})
	f.u.expect("POST", "/api/objects/"+archivedObject+"/archive", map[string]any{}, 204)
	previewPath := "/api/fields/" + textField + "/type-change/preview"
	request := map[string]any{"target_type": "integer"}
	preview := decodeMap(t, f.u.expect("POST", previewPath, request, 200))
	if preview["affected_objects"] != float64(3) || preview["saved_values"] != float64(3) || preview["archived_objects_affected"] != float64(1) || preview["incompatible_values"] != float64(1) || preview["can_apply"] != false {
		t.Fatalf("unexpected preview: %#v", preview)
	}
	var originalType string
	var originalValue string
	if err := f.e.db.QueryRow(`SELECT field_type FROM field_definitions WHERE id=?`, textField).Scan(&originalType); err != nil {
		t.Fatal(err)
	}
	if err := f.e.db.QueryRow(`SELECT value_json FROM field_values WHERE object_id=? AND field_id=?`, validObject, textField).Scan(&originalValue); err != nil {
		t.Fatal(err)
	}
	if originalType != "text" || originalValue != `"90.0"` {
		t.Fatalf("preview mutated data: type=%q value=%q", originalType, originalValue)
	}

	if _, err := f.e.db.Exec(`DELETE FROM field_values WHERE object_id=? AND field_id=?`, invalidObject, textField); err != nil {
		t.Fatal(err)
	}
	preview = decodeMap(t, f.u.expect("POST", previewPath, request, 200))
	apply := map[string]any{"target_type": "integer", "preview_token": preview["preview_token"]}
	f.u.expect("POST", "/api/fields/"+textField+"/type-change", apply, 200)
	if err := f.e.db.QueryRow(`SELECT field_type FROM field_definitions WHERE id=?`, textField).Scan(&originalType); err != nil {
		t.Fatal(err)
	}
	if err := f.e.db.QueryRow(`SELECT value_json FROM field_values WHERE object_id=? AND field_id=?`, validObject, textField).Scan(&originalValue); err != nil {
		t.Fatal(err)
	}
	if originalType != "integer" || originalValue != `90` {
		t.Fatalf("migration failed to preserve meaning: type=%q value=%q", originalType, originalValue)
	}
	if err := f.e.db.QueryRow(`SELECT value_json FROM field_values WHERE object_id=? AND field_id=?`, archivedObject, textField).Scan(&originalValue); err != nil || originalValue != `91` {
		t.Fatalf("archived value was not migrated: value=%q err=%v", originalValue, err)
	}
	var outcome string
	if err := f.e.db.QueryRow(`SELECT outcome FROM field_type_migrations WHERE field_id=? ORDER BY created_at DESC LIMIT 1`, textField).Scan(&outcome); err != nil || outcome != "applied" {
		t.Fatalf("migration audit outcome=%q err=%v", outcome, err)
	}
	var eventCount int
	if err := f.e.db.QueryRow(`SELECT COUNT(*) FROM object_history_events WHERE object_id=? AND event_type='field_type_migrated'`, validObject).Scan(&eventCount); err != nil || eventCount != 1 {
		t.Fatalf("history migration events=%d err=%v", eventCount, err)
	}
}

func TestFieldTypeMigrationRejectsStalePreviewAndDirectTypePatch(t *testing.T) {
	f := newImprovementFixture(t)
	textField := f.create(t, "/api/object-types/"+f.typ+"/fields", map[string]any{"name": "Rank text", "field_type": "text"})
	objectID := f.create(t, "/api/games/"+f.game+"/objects", map[string]any{"name": "Rank", "object_type_id": f.typ, "values": map[string]any{textField: "5"}})
	preview := decodeMap(t, f.u.expect("POST", "/api/fields/"+textField+"/type-change/preview", map[string]any{"target_type": "integer"}, 200))
	f.u.expect("PATCH", "/api/objects/"+objectID+"/values", map[string]any{"values": map[string]any{textField: "6"}}, 200)
	f.u.expect("POST", "/api/fields/"+textField+"/type-change", map[string]any{"target_type": "integer", "preview_token": preview["preview_token"]}, 409)
	f.u.expect("PATCH", "/api/fields/"+textField, map[string]any{"field_type": "integer"}, 409)
	if _, err := f.e.db.Exec(`UPDATE field_definitions SET field_type='integer' WHERE id=?`, textField); err == nil {
		t.Fatal("database trigger allowed a type change without an applying migration audit row")
	}
	var got string
	if err := f.e.db.QueryRow(`SELECT field_type FROM field_definitions WHERE id=?`, textField).Scan(&got); err != nil || got != "text" {
		t.Fatalf("stale preview changed field type: %q err=%v", got, err)
	}
}

func TestReferenceFieldMigrationRequiresAndPreservesExplicitSerialization(t *testing.T) {
	f := newImprovementFixture(t)
	linkField := f.create(t, "/api/object-types/"+f.typ+"/fields", map[string]any{"name": "Link", "field_type": "reference"})
	targetID := f.create(t, "/api/games/"+f.game+"/objects", map[string]any{"name": "Target", "object_type_id": f.typ})
	ownerID := f.create(t, "/api/games/"+f.game+"/objects", map[string]any{"name": "Owner", "object_type_id": f.typ, "values": map[string]any{linkField: targetID}})
	previewPath := "/api/fields/" + linkField + "/type-change/preview"
	base := map[string]any{"target_type": "long_text", "options": map[string]any{}}
	blocked := decodeMap(t, f.u.expect("POST", previewPath, base, 200))
	if blocked["can_apply"] != false || blocked["incompatible_values"] != float64(1) {
		t.Fatalf("relationship conversion should require explicit policy: %#v", blocked)
	}
	request := map[string]any{"target_type": "long_text", "options": map[string]any{}, "policy": map[string]any{"relationship_strategy": "serialize_ids"}}
	preview := decodeMap(t, f.u.expect("POST", previewPath, request, 200))
	if preview["can_apply"] != true {
		t.Fatalf("explicit serialization should be applicable: %#v", preview)
	}
	request["preview_token"] = preview["preview_token"]
	f.u.expect("POST", "/api/fields/"+linkField+"/type-change", request, 200)
	var linked int
	var stored string
	if err := f.e.db.QueryRow(`SELECT COUNT(*) FROM object_relationships WHERE object_id=? AND field_id=?`, ownerID, linkField).Scan(&linked); err != nil {
		t.Fatal(err)
	}
	if err := f.e.db.QueryRow(`SELECT value_json FROM field_values WHERE object_id=? AND field_id=?`, ownerID, linkField).Scan(&stored); err != nil {
		t.Fatal(err)
	}
	var serialized string
	if err := json.Unmarshal([]byte(stored), &serialized); err != nil {
		t.Fatal(err)
	}
	if linked != 0 || serialized != `[`+`"`+targetID+`"`+`]` {
		t.Fatalf("relationship was not preserved: linked=%d stored=%q", linked, serialized)
	}
}

func TestTextToReferenceMigrationRequiresMappedOwnedObjects(t *testing.T) {
	f := newImprovementFixture(t)
	textField := f.create(t, "/api/object-types/"+f.typ+"/fields", map[string]any{"name": "Target name", "field_type": "text"})
	targetID := f.create(t, "/api/games/"+f.game+"/objects", map[string]any{"name": "North Keep", "object_type_id": f.typ})
	ownerID := f.create(t, "/api/games/"+f.game+"/objects", map[string]any{"name": "Owner", "object_type_id": f.typ, "values": map[string]any{textField: "North Keep"}})
	previewPath := "/api/fields/" + textField + "/type-change/preview"
	base := map[string]any{"target_type": "reference", "options": map[string]any{"allowed_type_id": f.typ}}
	blocked := decodeMap(t, f.u.expect("POST", previewPath, base, 200))
	if blocked["can_apply"] != false || blocked["incompatible_values"] != float64(1) {
		t.Fatalf("unmapped text should be blocked: %#v", blocked)
	}
	request := map[string]any{"target_type": "reference", "options": map[string]any{"allowed_type_id": f.typ}, "policy": map[string]any{"reference_mapping": map[string]string{"North Keep": targetID}}}
	preview := decodeMap(t, f.u.expect("POST", previewPath, request, 200))
	if preview["can_apply"] != true {
		t.Fatalf("valid object mapping should be applicable: %#v", preview)
	}
	request["preview_token"] = preview["preview_token"]
	f.u.expect("POST", "/api/fields/"+textField+"/type-change", request, 200)
	var linked string
	if err := f.e.db.QueryRow(`SELECT referenced_object_id FROM object_relationships WHERE object_id=? AND field_id=?`, ownerID, textField).Scan(&linked); err != nil || linked != targetID {
		t.Fatalf("mapped relationship=%q want %q: %v", linked, targetID, err)
	}
	var stored int
	if err := f.e.db.QueryRow(`SELECT COUNT(*) FROM field_values WHERE object_id=? AND field_id=?`, ownerID, textField).Scan(&stored); err != nil || stored != 0 {
		t.Fatalf("source text row was not replaced with a relationship: count=%d err=%v", stored, err)
	}
}

func TestFormulaFreezeRequiresExplicitConfirmationAndStoresResults(t *testing.T) {
	f := newImprovementFixture(t)
	formulaField := f.create(t, "/api/object-types/"+f.typ+"/fields", map[string]any{"name": "Twice value", "field_type": "formula", "options": map[string]any{"expression": "@{" + f.number + "} * 2"}})
	previewPath := "/api/fields/" + formulaField + "/type-change/preview"
	base := map[string]any{"target_type": "integer", "options": map[string]any{}}
	blocked := decodeMap(t, f.u.expect("POST", previewPath, base, 200))
	if blocked["can_apply"] != false || blocked["formula_warning"] == nil {
		t.Fatalf("formula freeze must require explicit confirmation: %#v", blocked)
	}
	request := map[string]any{"target_type": "integer", "options": map[string]any{}, "policy": map[string]any{"freeze_formula": true}}
	preview := decodeMap(t, f.u.expect("POST", previewPath, request, 200))
	if preview["can_apply"] != true || preview["affected_objects"] != float64(1) {
		t.Fatalf("confirmed formula freeze should be ready: %#v", preview)
	}
	request["preview_token"] = preview["preview_token"]
	f.u.expect("POST", "/api/fields/"+formulaField+"/type-change", request, 200)
	var stored string
	if err := f.e.db.QueryRow(`SELECT value_json FROM field_values WHERE object_id=? AND field_id=?`, f.object, formulaField).Scan(&stored); err != nil || stored != `20` {
		t.Fatalf("frozen formula value=%q err=%v", stored, err)
	}
	f.u.expect("PATCH", "/api/objects/"+f.object+"/values", map[string]any{"values": map[string]any{f.number: 12}}, 200)
	if err := f.e.db.QueryRow(`SELECT value_json FROM field_values WHERE object_id=? AND field_id=?`, f.object, formulaField).Scan(&stored); err != nil || stored != `20` {
		t.Fatalf("formula freeze should not recalculate later: value=%q err=%v", stored, err)
	}
}

func TestFieldMigrationOwnershipAndDependencyChecks(t *testing.T) {
	f := newImprovementFixture(t)
	other := f.e.client()
	other.register("migration_other")
	other.expect("POST", "/api/fields/"+f.number+"/type-change/preview", map[string]any{"target_type": "text"}, 404)
	if _, err := f.e.db.Exec(`UPDATE object_types SET primary_display_field_id=? WHERE id=?`, f.number, f.typ); err != nil {
		t.Fatal(err)
	}
	preview := decodeMap(t, f.u.expect("POST", "/api/fields/"+f.number+"/type-change/preview", map[string]any{"target_type": "text"}, 200))
	if preview["can_apply"] != false || len(preview["dependency_conflicts"].([]any)) == 0 {
		t.Fatalf("cross-family primary display dependency should block migration: %#v", preview)
	}
}

func TestFieldMigrationBlocksUntilPresetOverridesAreResolved(t *testing.T) {
	f := newImprovementFixture(t)
	f.u.expect("POST", "/api/objects/"+f.object+"/presets", map[string]any{"name": "Snapshot", "values": map[string]any{f.number: 15}}, 201)
	preview := decodeMap(t, f.u.expect("POST", "/api/fields/"+f.number+"/type-change/preview", map[string]any{"target_type": "integer"}, 200))
	if preview["preset_overrides"] != float64(1) || preview["can_apply"] == true {
		t.Fatalf("migration should block rather than leave preset data invalid: %#v", preview)
	}
	found := false
	for _, raw := range preview["dependencies"].([]any) {
		if raw == "preset overrides" {
			found = true
		}
	}
	if !found {
		t.Fatalf("preset dependency was not explained: %#v", preview["dependencies"])
	}
}
