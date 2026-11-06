package httpapi

import (
	"encoding/json"
	"strconv"
	"strings"
	"testing"
)

func TestGameTemplateClonesStructureWithStableIDRemappingAndNoProgress(t *testing.T) {
	f := newImprovementFixture(t)
	otherType := f.create(t, "/api/games/"+f.game+"/object-types", map[string]any{"name": "Related type"})
	otherField := f.create(t, "/api/object-types/"+otherType+"/fields", map[string]any{"name": "Defaulted value", "field_type": "integer", "default_value": 7})
	referenceField := f.create(t, "/api/object-types/"+f.typ+"/fields", map[string]any{"name": "Related", "field_type": "reference", "options": map[string]any{"allowed_type_id": otherType}})
	formulaField := f.create(t, "/api/object-types/"+f.typ+"/fields", map[string]any{"name": "Doubled", "field_type": "formula", "options": map[string]any{"expression": "@{" + f.number + "} * 2"}})
	f.u.expect("PATCH", "/api/object-types/"+f.typ, map[string]any{"primary_display_field_id": f.number}, 200)
	rootSection := f.section(t, "Plan", nil)
	childSection := f.section(t, "Details", rootSection)
	f.u.expect("PATCH", "/api/sections/"+rootSection, map[string]any{"display_style": "table", "display_config": map[string]any{"visible_field_ids": []string{f.number}}}, 200)
	f.u.expect("PATCH", "/api/games/"+f.game, map[string]any{"personal_notes": "private playthrough notes"}, 200)
	goalID := f.create(t, "/api/games/"+f.game+"/goals", map[string]any{"title": "Personal milestone", "goal_type": "manual"})
	body := map[string]any{"name": "Reusable structure", "description": "Template notes", "include_sections": true, "include_types": true, "include_field_defaults": true, "include_presentation": true}
	template := decodeMap(t, f.u.expect("POST", "/api/games/"+f.game+"/templates", body, 201))
	templateID := mapString(template, "id")
	if template["type_count"] != float64(2) || template["section_count"] != float64(2) {
		t.Fatalf("template summary=%#v", template)
	}
	first := decodeMap(t, f.u.expect("POST", "/api/templates/"+templateID+"/instantiate", map[string]any{"name": "From template"}, 201))
	second := decodeMap(t, f.u.expect("POST", "/api/templates/"+templateID+"/instantiate", map[string]any{"name": "Second copy"}, 201))
	firstGame, secondGame := mapString(first, "id"), mapString(second, "id")
	if firstGame == "" || secondGame == "" || firstGame == secondGame {
		t.Fatalf("instantiation returned duplicate game IDs: %q %q", firstGame, secondGame)
	}
	var objectCount, goalCount int
	if err := f.e.db.QueryRow(`SELECT COUNT(*) FROM objects WHERE game_id=?`, firstGame).Scan(&objectCount); err != nil {
		t.Fatal(err)
	}
	if err := f.e.db.QueryRow(`SELECT COUNT(*) FROM goals WHERE game_id=?`, firstGame).Scan(&goalCount); err != nil {
		t.Fatal(err)
	}
	if objectCount != 0 || goalCount != 0 {
		t.Fatalf("template copied personal data: objects=%d goals=%d", objectCount, goalCount)
	}
	var notes, newTypeID, copiedPrimary string
	if err := f.e.db.QueryRow(`SELECT personal_notes FROM games WHERE id=?`, firstGame).Scan(&notes); err != nil || notes != "" {
		t.Fatalf("private notes copied: %q err=%v", notes, err)
	}
	if err := f.e.db.QueryRow(`SELECT id,primary_display_field_id FROM object_types WHERE game_id=? AND name='Custom type'`, firstGame).Scan(&newTypeID, &copiedPrimary); err != nil {
		t.Fatal(err)
	}
	if newTypeID == f.typ || copiedPrimary == f.number {
		t.Fatalf("type/field ID was reused: type=%q primary=%q", newTypeID, copiedPrimary)
	}
	var newNumberID, newFormulaID, newReferenceID string
	if err := f.e.db.QueryRow(`SELECT id FROM field_definitions WHERE object_type_id=? AND name='Custom value'`, newTypeID).Scan(&newNumberID); err != nil {
		t.Fatal(err)
	}
	if err := f.e.db.QueryRow(`SELECT id FROM field_definitions WHERE object_type_id=? AND name='Doubled'`, newTypeID).Scan(&newFormulaID); err != nil {
		t.Fatal(err)
	}
	if newNumberID == f.number || copiedPrimary != newNumberID || newFormulaID == formulaField {
		t.Fatalf("field IDs or primary display were not remapped: number=%q primary=%q formula=%q", newNumberID, copiedPrimary, newFormulaID)
	}
	var optionsRaw string
	if err := f.e.db.QueryRow(`SELECT options_json FROM field_definitions WHERE id=?`, newFormulaID).Scan(&optionsRaw); err != nil {
		t.Fatal(err)
	}
	if strings.Contains(optionsRaw, f.number) || !strings.Contains(optionsRaw, newNumberID) {
		t.Fatalf("formula references were not remapped: %s", optionsRaw)
	}
	if err := f.e.db.QueryRow(`SELECT id FROM field_definitions WHERE object_type_id=? AND name='Related'`, newTypeID).Scan(&newReferenceID); err != nil {
		t.Fatal(err)
	}
	if newReferenceID == referenceField {
		t.Fatal("reference field ID was reused")
	}
	if err := f.e.db.QueryRow(`SELECT options_json FROM field_definitions WHERE id=?`, newReferenceID).Scan(&optionsRaw); err != nil {
		t.Fatal(err)
	}
	var refOptions map[string]string
	if err := json.Unmarshal([]byte(optionsRaw), &refOptions); err != nil || refOptions["allowed_type_id"] == otherType || refOptions["allowed_type_id"] == "" {
		t.Fatalf("allowed type mapping=%#v err=%v", refOptions, err)
	}
	var sectionCount int
	if err := f.e.db.QueryRow(`SELECT COUNT(*) FROM sections WHERE game_id=?`, firstGame).Scan(&sectionCount); err != nil || sectionCount != 2 {
		t.Fatalf("sections copied=%d err=%v", sectionCount, err)
	}
	var parent string
	if err := f.e.db.QueryRow(`SELECT parent_id FROM sections WHERE game_id=? AND name='Details'`, firstGame).Scan(&parent); err != nil || parent == rootSection || parent == childSection || parent == "" {
		t.Fatalf("nested section was not remapped: parent=%q err=%v", parent, err)
	}
	var config string
	if err := f.e.db.QueryRow(`SELECT display_config FROM sections WHERE game_id=? AND name='Plan'`, firstGame).Scan(&config); err != nil || strings.Contains(config, f.number) || !strings.Contains(config, newNumberID) {
		t.Fatalf("section field configuration not remapped: %s err=%v", config, err)
	}
	var newOtherType, copiedDefault, copiedDefaultFieldID string
	if err := f.e.db.QueryRow(`SELECT id FROM object_types WHERE game_id=? AND name='Related type'`, firstGame).Scan(&newOtherType); err != nil {
		t.Fatal(err)
	}
	if err := f.e.db.QueryRow(`SELECT id,default_json FROM field_definitions WHERE object_type_id=? AND name='Defaulted value'`, newOtherType).Scan(&copiedDefaultFieldID, &copiedDefault); err != nil || copiedDefault != `7` || copiedDefaultFieldID == otherField {
		t.Fatalf("selected field default not copied: %q err=%v", copiedDefault, err)
	}
	var sourceGoalCount int
	if err := f.e.db.QueryRow(`SELECT COUNT(*) FROM goals WHERE game_id=? AND id=?`, f.game, goalID).Scan(&sourceGoalCount); err != nil || sourceGoalCount != 1 {
		t.Fatalf("source game goal was changed: count=%d err=%v", sourceGoalCount, err)
	}
}

func TestTemplatesSupportPortableExportImportAndOwnership(t *testing.T) {
	f := newImprovementFixture(t)
	templateID := mapString(decodeMap(t, f.u.expect("POST", "/api/games/"+f.game+"/templates", map[string]any{"name": "Portable"}, 201)), "id")
	var exported map[string]any
	if err := json.Unmarshal(f.u.expect("GET", "/api/templates/"+templateID+"/export", nil, 200), &exported); err != nil {
		t.Fatal(err)
	}
	imported := decodeMap(t, f.u.expect("POST", "/api/templates/import", exported, 201))
	importedID := mapString(imported, "id")
	other := f.e.client()
	other.register("template_foreign")
	other.expect("GET", "/api/templates/"+templateID, nil, 404)
	other.expect("POST", "/api/templates/"+templateID+"/instantiate", map[string]any{"name": "Blocked"}, 404)
	f.u.expect("PATCH", "/api/templates/"+importedID, map[string]any{"name": "Renamed portable"}, 200)
	duplicate := decodeMap(t, f.u.expect("POST", "/api/templates/"+importedID+"/duplicate", map[string]any{}, 201))
	if mapString(duplicate, "id") == importedID {
		t.Fatal("duplicate template reused its ID")
	}
	f.u.expect("DELETE", "/api/templates/"+mapString(duplicate, "id"), nil, 204)
	f.u.expect("POST", "/api/templates/import", map[string]any{"version": 999, "name": "Unsupported", "structure": map[string]any{"version": 999}}, 400)
}

func TestTemplateWithoutDefaultsOrSectionsOmitsPresentationState(t *testing.T) {
	f := newImprovementFixture(t)
	field := f.create(t, "/api/object-types/"+f.typ+"/fields", map[string]any{"name": "Defaulted", "field_type": "text", "default_value": "private default"})
	section := f.section(t, "Presentation", nil)
	f.u.expect("PATCH", "/api/object-types/"+f.typ, map[string]any{"primary_display_field_id": field}, 200)
	templateID := mapString(decodeMap(t, f.u.expect("POST", "/api/games/"+f.game+"/templates", map[string]any{"name": "Structure only", "include_sections": false, "include_field_defaults": false, "include_presentation": false}, 201)), "id")
	created := decodeMap(t, f.u.expect("POST", "/api/templates/"+templateID+"/instantiate", map[string]any{"name": "Sparse"}, 201))
	gameID := mapString(created, "id")
	var defaultRows, sectionRows, primaryRows int
	if err := f.e.db.QueryRow(`SELECT COUNT(*) FROM field_definitions f JOIN object_types t ON t.id=f.object_type_id WHERE t.game_id=? AND f.name='Defaulted' AND f.default_json IS NOT NULL`, gameID).Scan(&defaultRows); err != nil {
		t.Fatal(err)
	}
	if err := f.e.db.QueryRow(`SELECT COUNT(*) FROM sections WHERE game_id=?`, gameID).Scan(&sectionRows); err != nil {
		t.Fatal(err)
	}
	if err := f.e.db.QueryRow(`SELECT COUNT(*) FROM object_types WHERE game_id=? AND primary_display_field_id<>''`, gameID).Scan(&primaryRows); err != nil {
		t.Fatal(err)
	}
	if defaultRows != 0 || sectionRows != 0 || primaryRows != 0 || section == "" {
		t.Fatalf("unchecked structures leaked: defaults=%d sections=%d primary=%d", defaultRows, sectionRows, primaryRows)
	}
}

func TestTemplateLibraryLimitAppliesToCreateImportAndDuplicate(t *testing.T) {
	f := newImprovementFixture(t)
	templateID := mapString(decodeMap(t, f.u.expect("POST", "/api/games/"+f.game+"/templates", map[string]any{"name": "At limit"}, 201)), "id")
	var exported map[string]any
	if err := json.Unmarshal(f.u.expect("GET", "/api/templates/"+templateID+"/export", nil, 200), &exported); err != nil {
		t.Fatal(err)
	}
	var userID string
	if err := f.e.db.QueryRow(`SELECT id FROM users WHERE username=?`, "improvements").Scan(&userID); err != nil {
		t.Fatal(err)
	}
	const document = `{"version":1,"game_icon":"🎮","game_description":"","object_types":[],"sections":[]}`
	for i := 0; i < 99; i++ {
		if _, err := f.e.db.Exec(`INSERT INTO game_templates(id,user_id,name,description,icon,template_version,data_json,created_at,updated_at) VALUES(?,?,?,'','📘',1,?,?,?)`, "filler-"+strconv.Itoa(i), userID, "Filler", document, "now", "now"); err != nil {
			t.Fatal(err)
		}
	}
	f.u.expect("POST", "/api/games/"+f.game+"/templates", map[string]any{"name": "Too many"}, 409)
	f.u.expect("POST", "/api/templates/"+templateID+"/duplicate", map[string]any{}, 409)
	f.u.expect("POST", "/api/templates/import", exported, 409)
}
