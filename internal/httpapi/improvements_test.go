package httpapi

import (
	"encoding/json"
	"fmt"
	"net/http"
	"net/url"
	"strconv"
	"strings"
	"testing"
)

type improvementFixture struct {
	e                         *testEnvironment
	u                         *testUser
	game, typ, number, object string
}

func newImprovementFixture(t *testing.T) improvementFixture {
	e := newTestEnvironment(t)
	u := e.client()
	u.register("improvements")
	create := func(path string, body any) string {
		return mapString(decodeMap(t, u.expect("POST", path, body, 201)), "id")
	}
	g := create("/api/games", map[string]any{"name": "Custom game"})
	typ := create("/api/games/"+g+"/object-types", map[string]any{"name": "Custom type"})
	n := create("/api/object-types/"+typ+"/fields", map[string]any{"name": "Custom value", "field_type": "decimal"})
	obj := create("/api/games/"+g+"/objects", map[string]any{"name": "Example", "object_type_id": typ, "values": map[string]any{n: 10}})
	return improvementFixture{e, u, g, typ, n, obj}
}
func (f improvementFixture) create(t *testing.T, path string, body any) string {
	t.Helper()
	return mapString(decodeMap(t, f.u.expect("POST", path, body, 201)), "id")
}
func (f improvementFixture) section(t *testing.T, name string, parent any) string {
	return f.create(t, "/api/games/"+f.game+"/sections", map[string]any{"name": name, "parent_id": parent})
}
func (f improvementFixture) goals(t *testing.T) []goal {
	var result struct {
		Items []goal `json:"items"`
	}
	if err := json.Unmarshal(f.u.expect("GET", "/api/games/"+f.game+"/goals", nil, 200), &result); err != nil {
		t.Fatal(err)
	}
	return result.Items
}

func TestSectionMovesPreserveHierarchyPlacementsAndArchive(t *testing.T) {
	f := newImprovementFixture(t)
	a := f.section(t, "A", nil)
	b := f.section(t, "B", nil)
	child := f.section(t, "Child", a)
	leaf := f.section(t, "Leaf", child)
	f.u.expect("POST", "/api/sections/"+child+"/placements", map[string]any{"object_id": f.object}, 204)
	goalID := f.create(t, "/api/games/"+f.game+"/goals", map[string]any{"title": "Keep placement", "goal_type": "manual"})
	f.u.expect("POST", "/api/sections/"+child+"/goals", map[string]any{"goal_id": goalID}, 204)
	move := func(id string, parent any, before string, want int) {
		t.Helper()
		f.u.expect("POST", "/api/sections/"+id+"/move", map[string]any{"parent_id": parent, "before_id": before}, want)
	}
	move(a, a, "", 400)
	move(a, leaf, "", 400)
	other := f.create(t, "/api/games", map[string]any{"name": "Other game"})
	foreign := f.create(t, "/api/games/"+other+"/sections", map[string]any{"name": "Foreign"})
	move(child, foreign, "", 400)
	move(child, nil, foreign, 400)
	move(b, nil, a, 204)
	var orderA, orderB int
	if err := f.e.db.QueryRow(`SELECT sort_order FROM sections WHERE id=?`, a).Scan(&orderA); err != nil {
		t.Fatal(err)
	}
	if err := f.e.db.QueryRow(`SELECT sort_order FROM sections WHERE id=?`, b).Scan(&orderB); err != nil {
		t.Fatal(err)
	}
	if orderB >= orderA {
		t.Fatal("root reorder was not persisted")
	}
	move(child, b, "", 204)
	var parent string
	if err := f.e.db.QueryRow(`SELECT parent_id FROM sections WHERE id=?`, child).Scan(&parent); err != nil || parent != b {
		t.Fatalf("parent=%q, err=%v", parent, err)
	}
	move(child, nil, a, 204)
	f.u.expect("DELETE", "/api/sections/"+child, nil, 204)
	move(leaf, child, "", 400)
	var objects, goals, activeLeaf int
	f.e.db.QueryRow(`SELECT COUNT(*) FROM section_placements WHERE section_id=? AND object_id=?`, child, f.object).Scan(&objects)
	f.e.db.QueryRow(`SELECT COUNT(*) FROM section_goal_placements WHERE section_id=? AND goal_id=?`, child, goalID).Scan(&goals)
	f.e.db.QueryRow(`SELECT COUNT(*) FROM sections WHERE id=? AND parent_id=? AND archived=0`, leaf, child).Scan(&activeLeaf)
	if objects != 1 || goals != 1 || activeLeaf != 1 {
		t.Fatalf("archive changed placements or children: %d %d %d", objects, goals, activeLeaf)
	}
	f.u.expect("POST", "/api/sections/"+child+"/restore", map[string]any{}, 204)
	var result struct {
		Items []section `json:"items"`
	}
	json.Unmarshal(f.u.expect("GET", "/api/games/"+f.game+"/sections", nil, 200), &result)
	if len(result.Items) != 4 {
		t.Fatalf("restored tree has %d sections", len(result.Items))
	}
	outsider := f.e.client()
	outsider.register("outsider")
	outsider.expect("POST", "/api/sections/"+child+"/move", map[string]any{"parent_id": nil}, 404)
}

func TestGoalEditingPreservesIDsTasksPlacementsAndCreationTime(t *testing.T) {
	f := newImprovementFixture(t)
	id := f.create(t, "/api/games/"+f.game+"/goals", map[string]any{"title": "Original", "goal_type": "checklist", "tasks": []string{"First", "Second", "Delete me"}})
	section := f.section(t, "Goals", nil)
	f.u.expect("POST", "/api/sections/"+section+"/goals", map[string]any{"goal_id": id}, 204)
	before := f.goals(t)[0]
	tasks := []goalTask{{ID: before.Tasks[1].ID, Title: "Renamed second", Completed: true}, {ID: before.Tasks[0].ID, Title: "First"}, {Title: "New task", Completed: true}}
	var updated goal
	if err := json.Unmarshal(f.u.expect("PATCH", "/api/goals/"+id, map[string]any{"title": "Updated", "description": "New description", "tasks": tasks}, 200), &updated); err != nil {
		t.Fatal(err)
	}
	if updated.ID != id || updated.CreatedAt != before.CreatedAt || len(updated.SectionIDs) != 1 || updated.SectionIDs[0] != section {
		t.Fatal("goal identity, creation time or placement changed")
	}
	if len(updated.Tasks) != 3 || updated.Tasks[0].ID != before.Tasks[1].ID || updated.Tasks[1].ID != before.Tasks[0].ID || updated.Tasks[2].ID == "" || updated.Tasks[0].Title != "Renamed second" {
		t.Fatalf("task edit: %+v", updated.Tasks)
	}
	if updated.Current != 0 || updated.Completed || updated.Progress < 66 || updated.Progress > 67 {
		t.Fatalf("progress=%v", updated.Progress)
	}
	f.u.expect("PATCH", "/api/goals/"+id, map[string]any{"tasks": []goalTask{{ID: "foreign", Title: "Bad"}}, "title": "Do not save"}, 400)
	if f.goals(t)[0].Title != "Updated" {
		t.Fatal("failed task edit did not roll back title")
	}
	f.u.expect("PATCH", "/api/goals/"+id, map[string]any{"current": 10}, 409)
	f.u.expect("PATCH", "/api/goals/"+id, map[string]any{"tasks": []goalTask{}}, 400)
	f.u.expect("PATCH", "/api/goals/"+id, map[string]any{"goal_type": "manual"}, 400)
	numeric := f.create(t, "/api/games/"+f.game+"/goals", map[string]any{"title": "Numeric", "goal_type": "numeric", "target": 100, "current": 20})
	data := decodeMap(t, f.u.expect("PATCH", "/api/goals/"+numeric, map[string]any{"target": 25, "current": 25}, 200))
	if data["completed"] != true || data["progress"] != float64(100) {
		t.Fatal("numeric progress not recalculated")
	}
	for _, target := range []any{0, -1, "bad", nil} {
		f.u.expect("PATCH", "/api/goals/"+numeric, map[string]any{"target": target}, 400)
	}
	f.u.expect("PATCH", "/api/goals/"+numeric, map[string]any{"current": -1}, 400)
	manual := f.create(t, "/api/games/"+f.game+"/goals", map[string]any{"title": "Manual", "goal_type": "manual"})
	data = decodeMap(t, f.u.expect("PATCH", "/api/goals/"+manual, map[string]any{"title": "Done", "description": "Edited", "completed": true}, 200))
	if data["completed"] != true {
		t.Fatal("manual completion did not save")
	}
}

func TestGoalCreationSavesInitialTaskStatesAtomically(t *testing.T) {
	f := newImprovementFixture(t)
	id := f.create(t, "/api/games/"+f.game+"/goals", map[string]any{"title": "Checklist", "goal_type": "checklist", "task_details": []goalTask{{Title: "Done", Completed: true}, {Title: "Pending"}}})
	item := f.goals(t)[0]
	if item.ID != id || len(item.Tasks) != 2 || !item.Tasks[0].Completed || item.Progress != 50 {
		t.Fatalf("initial tasks: %+v", item)
	}
	f.u.expect("POST", "/api/games/"+f.game+"/goals", map[string]any{"title": "Invalid", "goal_type": "checklist", "task_details": []goalTask{{Title: "Valid"}, {Title: ""}}}, 400)
	if len(f.goals(t)) != 1 {
		t.Fatal("invalid checklist partially saved")
	}
	manual := f.create(t, "/api/games/"+f.game+"/goals", map[string]any{"title": "Already done", "goal_type": "manual", "completed": true})
	for _, goal := range f.goals(t) {
		if goal.ID == manual && !goal.Completed {
			t.Fatal("initial manual completion lost")
		}
	}
}

func TestGoalRuleEditingAndLegacyTargets(t *testing.T) {
	f := newImprovementFixture(t)
	makeCondition := func(value any) map[string]any {
		return map[string]any{"object_id": f.object, "field_id": f.number, "operator": "gte", "value": value}
	}
	id := f.create(t, "/api/games/"+f.game+"/goals", map[string]any{"title": "Rules", "goal_type": "field_target", "mode": "all", "conditions": []any{makeCondition(5), makeCondition(20)}})
	initial := f.goals(t)[0]
	if initial.Completed {
		t.Fatal("ALL completed incorrectly")
	}
	if len(initial.ConditionDetails) != 2 || !initial.ConditionDetails[0].Matched || initial.ConditionDetails[0].State != "satisfied" || initial.ConditionDetails[0].ObjectName != "Example" || initial.ConditionDetails[0].FieldName != "Custom value" || initial.ConditionDetails[1].State != "not_satisfied" {
		t.Fatalf("goal condition details should report names, live values, and status: %#v", initial.ConditionDetails)
	}
	data := decodeMap(t, f.u.expect("PATCH", "/api/goals/"+id, map[string]any{"mode": "any"}, 200))
	if data["completed"] != true {
		t.Fatal("ANY did not recalculate")
	}
	f.u.expect("PATCH", "/api/goals/"+id, map[string]any{"conditions": []any{makeCondition(30)}, "mode": "all"}, 200)
	if f.goals(t)[0].Completed {
		t.Fatal("updated condition was ignored")
	}
	f.u.expect("PATCH", "/api/goals/"+id, map[string]any{"conditions": []any{}}, 400)
	f.u.expect("PATCH", "/api/goals/"+id, map[string]any{"conditions": []any{makeCondition("bad")}}, 400)
	f.u.expect("PATCH", "/api/goals/"+id, map[string]any{"completed": true}, 409)
	f.u.expect("PATCH", "/api/goals/"+id, map[string]any{"current": 1}, 409)
	other := f.create(t, "/api/games", map[string]any{"name": "Foreign"})
	foreignType := f.create(t, "/api/games/"+other+"/object-types", map[string]any{"name": "Foreign type"})
	foreignField := f.create(t, "/api/object-types/"+foreignType+"/fields", map[string]any{"name": "Foreign field", "field_type": "decimal"})
	condition := makeCondition(1)
	condition["field_id"] = foreignField
	f.u.expect("PATCH", "/api/goals/"+id, map[string]any{"conditions": []any{condition}}, 400)
	legacy := f.create(t, "/api/games/"+f.game+"/goals", map[string]any{"title": "Legacy", "goal_type": "field_target", "object_id": f.object, "field_id": f.number, "target": 5})
	data = decodeMap(t, f.u.expect("PATCH", "/api/goals/"+legacy, map[string]any{"target": 15}, 200))
	if data["id"] != legacy || data["completed"] != false {
		t.Fatal("legacy target update lost identity or calculation")
	}
	broken := f.create(t, "/api/object-types/"+f.typ+"/fields", map[string]any{"name": "Broken formula", "field_type": "formula", "options": map[string]any{"expression": "@{" + f.number + "} / 0"}})
	invalidCondition := makeCondition(1)
	invalidCondition["field_id"] = broken
	invalidGoal := f.create(t, "/api/games/"+f.game+"/goals", map[string]any{"title": "Formula status", "goal_type": "field_target", "mode": "all", "conditions": []any{invalidCondition}})
	foundInvalid := false
	for _, item := range f.goals(t) {
		if item.ID == invalidGoal {
			foundInvalid = true
			if len(item.ConditionDetails) != 1 || item.ConditionDetails[0].State != "invalid_value" || !strings.Contains(item.ConditionDetails[0].Error, "division by zero") {
				t.Fatalf("formula failures should be identified in the goal condition details: %#v", item.ConditionDetails)
			}
		}
	}
	if !foundInvalid {
		t.Fatal("formula condition goal was not returned")
	}
}

func TestPrimaryDisplayAndFormulaPreview(t *testing.T) {
	f := newImprovementFixture(t)
	path := "/api/object-types/" + f.typ
	f.u.expect("PATCH", path, map[string]any{"primary_display_field_id": f.number}, 200)
	f.u.expect("PATCH", path, map[string]any{"primary_display_field_id": "foreign"}, 400)
	f.u.expect("POST", "/api/fields/"+f.number+"/archive", map[string]any{}, 204)
	if decodeMap(t, f.u.expect("GET", path, nil, 200))["primary_display_field_id"] != f.number {
		t.Fatal("archived primary selection was lost")
	}
	f.u.expect("PATCH", path, map[string]any{"name": "Rename while archived", "primary_display_field_id": f.number}, 200)
	f.u.expect("POST", "/api/fields/"+f.number+"/restore", map[string]any{}, 204)
	f.u.expect("PATCH", path, map[string]any{"primary_display_field_id": ""}, 200)
	if decodeMap(t, f.u.expect("GET", path, nil, 200))["primary_display_field_id"] != "" {
		t.Fatal("primary selection not cleared")
	}
	preview := func(expression string, objectID string, values any, want int) map[string]any {
		return decodeMap(t, f.u.expect("POST", path+"/formula-preview", map[string]any{"options": map[string]any{"expression": expression}, "object_id": objectID, "values": values}, want))
	}
	expression := "@{" + f.number + "} * 2"
	result := preview(expression, f.object, nil, 200)
	if result["result"] != float64(20) {
		t.Fatalf("preview=%v", result)
	}
	result = preview(expression, "", map[string]any{f.number: 7}, 200)
	if result["result"] != float64(14) {
		t.Fatal(result)
	}
	result = preview(expression, "", nil, 200)
	if result["error"] == nil || result["result"] != nil {
		t.Fatal("missing values silently became zero")
	}
	result = preview("@{"+f.number+"} / 0", f.object, nil, 200)
	if result["error"] != "division by zero" {
		t.Fatal(result)
	}
	preview("@{missing} + 1", f.object, nil, 400)
	preview("@{"+f.number+"} ** 2", f.object, nil, 400)
	calculated := f.create(t, path+"/fields", map[string]any{"name": "Calculated", "field_type": "formula", "options": map[string]any{"expression": expression}})
	f.u.expect("PATCH", "/api/fields/"+f.number, map[string]any{"name": "Renamed input"}, 200)
	record := decodeMap(t, f.u.expect("GET", "/api/objects/"+f.object, nil, 200))
	if record["values"].(map[string]any)[calculated] != float64(20) {
		t.Fatal("rename broke saved formula")
	}
	record = decodeMap(t, f.u.expect("GET", "/api/objects/"+f.object, nil, 200))
	if record["values"].(map[string]any)[f.number] != float64(10) {
		t.Fatal("preview mutated object")
	}
	f.u.expect("POST", path+"/formula-preview", map[string]any{"field_id": calculated, "options": map[string]any{"expression": "@{" + calculated + "}"}}, 400)
	// Two-step cycle through existing formulas.
	another := f.create(t, path+"/fields", map[string]any{"name": "Dependent", "field_type": "formula", "options": map[string]any{"expression": "@{" + calculated + "} + 1"}})
	f.u.expect("PATCH", "/api/fields/"+calculated, map[string]any{"options": map[string]any{"expression": "@{" + another + "}"}}, 400)
	f.u.expect("POST", path+"/fields", map[string]any{"name": "Invalid syntax", "field_type": "formula", "options": map[string]any{"expression": "@{" + f.number + "} +"}}, http.StatusBadRequest)
}

func TestSavedObjectViewValidationAndRoundTrip(t *testing.T) {
	f := newImprovementFixture(t)
	sectionID := f.section(t, "Filtered", nil)
	view := map[string]any{"search": "Example", "type_id": f.typ, "mode": "all", "sort": f.number, "direction": "desc", "conditions": []any{map[string]any{"field_id": f.number, "operator": "between", "value": 5, "value_to": 15}}}
	config := map[string]any{"object_view": view}
	f.u.expect("PATCH", "/api/sections/"+sectionID, map[string]any{"display_config": config}, 200)
	var result struct {
		Items []section `json:"items"`
	}
	json.Unmarshal(f.u.expect("GET", "/api/games/"+f.game+"/sections", nil, 200), &result)
	var saved struct {
		View objectView `json:"object_view"`
	}
	json.Unmarshal(result.Items[0].DisplayConfig, &saved)
	if saved.View.TypeID != f.typ || saved.View.Sort != f.number || len(saved.View.Conditions) != 1 {
		t.Fatal("saved view did not survive read")
	}
	view["conditions"] = []any{map[string]any{"field_id": f.number, "operator": "contains", "value": "bad"}}
	f.u.expect("PATCH", "/api/sections/"+sectionID, map[string]any{"display_config": config}, 400)
	view["conditions"] = []any{map[string]any{"field_id": "foreign", "operator": "eq", "value": 1}}
	f.u.expect("PATCH", "/api/sections/"+sectionID, map[string]any{"display_config": config}, 400)
	view["conditions"] = []any{map[string]any{"field_id": f.number, "operator": "between", "value": 20, "value_to": 10}}
	f.u.expect("PATCH", "/api/sections/"+sectionID, map[string]any{"display_config": config}, 400)
	var raw string
	f.e.db.QueryRow(`SELECT value_json FROM field_values WHERE object_id=? AND field_id=?`, f.object, f.number).Scan(&raw)
	if raw != "10" {
		t.Fatalf("filter changed object data: %s", raw)
	}
}

func TestFieldArchiveBlocksSavedViewDependenciesIncludingArchivedSections(t *testing.T) {
	f := newImprovementFixture(t)
	active := f.section(t, "Main Characters", nil)
	archived := f.section(t, "Old Builds", nil)
	view := map[string]any{
		"search":     "",
		"type_id":    f.typ,
		"mode":       "all",
		"sort":       f.number,
		"direction":  "desc",
		"conditions": []any{map[string]any{"field_id": f.number, "operator": "gte", "value": 5}},
	}
	config := map[string]any{"object_view": view}
	f.u.expect("PATCH", "/api/sections/"+active, map[string]any{"display_config": config}, 200)
	f.u.expect("PATCH", "/api/sections/"+archived, map[string]any{"display_config": config}, 200)
	f.u.expect("DELETE", "/api/sections/"+archived, nil, 204)

	result := decodeMap(t, f.u.expect("POST", "/api/fields/"+f.number+"/archive", map[string]any{}, 409))
	message := result["error"].(map[string]any)["message"].(string)
	for _, expected := range []string{"Custom value", "Main Characters: saved object view: filter", "Main Characters: saved object view: sorting", "archived section: Old Builds"} {
		if !strings.Contains(message, expected) {
			t.Fatalf("archive error should identify %q, got %q", expected, message)
		}
	}

	// A deliberately invalid foreign-game configuration must not affect this game's field.
	otherGame := f.create(t, "/api/games", map[string]any{"name": "Separate game"})
	otherSection := f.create(t, "/api/games/"+otherGame+"/sections", map[string]any{"name": "Foreign section"})
	raw, err := json.Marshal(config)
	if err != nil {
		t.Fatal(err)
	}
	if _, err := f.e.db.Exec(`UPDATE sections SET display_config=? WHERE id=?`, string(raw), otherSection); err != nil {
		t.Fatal(err)
	}
	if _, err := f.e.db.Exec(`UPDATE sections SET display_config='{}' WHERE id IN (?,?)`, active, archived); err != nil {
		t.Fatal(err)
	}
	f.u.expect("POST", "/api/fields/"+f.number+"/archive", map[string]any{}, 204)
	f.u.expect("POST", "/api/fields/"+f.number+"/restore", map[string]any{}, 204)
	object := decodeMap(t, f.u.expect("GET", "/api/objects/"+f.object, nil, 200))
	if object["values"].(map[string]any)[f.number] != float64(10) {
		t.Fatalf("field archive and restore should preserve its saved value: %#v", object["values"])
	}
}

func TestFieldArchiveBlocksTransitiveFormulaDependencies(t *testing.T) {
	f := newImprovementFixture(t)
	first := f.create(t, "/api/object-types/"+f.typ+"/fields", map[string]any{"name": "First formula", "field_type": "formula", "options": map[string]any{"expression": "@{" + f.number + "} + 1"}})
	f.create(t, "/api/object-types/"+f.typ+"/fields", map[string]any{"name": "Second formula", "field_type": "formula", "options": map[string]any{"expression": "@{" + first + "} + 1"}})
	result := decodeMap(t, f.u.expect("POST", "/api/fields/"+f.number+"/archive", map[string]any{}, 409))
	dependencies := result["dependencies"].([]any)
	if len(dependencies) != 2 {
		t.Fatalf("expected direct and transitive formula dependencies, got %#v", dependencies)
	}
}

func TestObjectQueriesReturnGloballySortedFilteredPagesAndTotals(t *testing.T) {
	f := newImprovementFixture(t)
	alpha := f.create(t, "/api/games/"+f.game+"/objects", map[string]any{"name": "Alpha", "object_type_id": f.typ, "values": map[string]any{f.number: 3}})
	beta := f.create(t, "/api/games/"+f.game+"/objects", map[string]any{"name": "Beta", "object_type_id": f.typ, "values": map[string]any{f.number: 7}})
	page := decodeMap(t, f.u.expect("GET", "/api/games/"+f.game+"/objects?limit=1&offset=1&sort=name&direction=asc", nil, 200))
	items := page["items"].([]any)
	if page["total"] != float64(3) || len(items) != 1 || items[0].(map[string]any)["id"] != beta {
		t.Fatalf("name sorting must happen before pagination and report the global total: %#v", page)
	}
	search := decodeMap(t, f.u.expect("GET", "/api/games/"+f.game+"/objects?search=aLpHa&limit=10", nil, 200))
	if search["total"] != float64(1) || search["items"].([]any)[0].(map[string]any)["id"] != alpha {
		t.Fatalf("server search should be case insensitive and complete: %#v", search)
	}
	section := f.section(t, "Selected", nil)
	f.u.expect("POST", "/api/sections/"+section+"/placements", map[string]any{"object_id": beta}, 204)
	sectionPage := decodeMap(t, f.u.expect("GET", "/api/games/"+f.game+"/objects?section_id="+section+"&limit=20", nil, 200))
	if sectionPage["total"] != float64(1) || sectionPage["items"].([]any)[0].(map[string]any)["id"] != beta {
		t.Fatalf("section query should return only shared objects placed there: %#v", sectionPage)
	}
	filters, err := json.Marshal(map[string]any{"mode": "all", "conditions": []any{map[string]any{"field_id": f.number, "operator": "gte", "value": 5}}})
	if err != nil {
		t.Fatal(err)
	}
	query := "/api/games/" + f.game + "/objects?limit=1&type_id=" + url.QueryEscape(f.typ) + "&sort=" + url.QueryEscape(f.number) + "&direction=desc&filters=" + url.QueryEscape(string(filters))
	filtered := decodeMap(t, f.u.expect("GET", query, nil, 200))
	if filtered["total"] != float64(2) || filtered["items"].([]any)[0].(map[string]any)["id"] != f.object || filtered["has_more"] != true {
		t.Fatalf("custom field filtering and sorting should cover the full matching set: %#v", filtered)
	}
	planRows, err := f.e.db.Query(`EXPLAIN QUERY PLAN SELECT o.id FROM objects o JOIN object_types t ON t.id=o.object_type_id WHERE o.game_id=? AND o.object_type_id=? AND EXISTS (SELECT 1 FROM field_definitions fd LEFT JOIN field_values fv ON fv.object_id=o.id AND fv.field_id=fd.id WHERE fd.id=? AND fd.object_type_id=o.object_type_id AND fd.archived=0 AND json_type(CASE WHEN fv.value_json IS NOT NULL THEN fv.value_json ELSE fd.default_json END,'$') IN ('integer','real') AND CAST(json_extract(CASE WHEN fv.value_json IS NOT NULL THEN fv.value_json ELSE fd.default_json END,'$') AS REAL)>=?)`, f.game, f.typ, f.number, 5)
	if err != nil {
		t.Fatal(err)
	}
	var plan []string
	for planRows.Next() {
		var id, parent, unused int
		var detail string
		if err := planRows.Scan(&id, &parent, &unused, &detail); err != nil {
			planRows.Close()
			t.Fatal(err)
		}
		plan = append(plan, detail)
	}
	if err := planRows.Close(); err != nil {
		t.Fatal(err)
	}
	planText := strings.Join(plan, "\n")
	if !strings.Contains(planText, "SEARCH fv USING INDEX") {
		t.Fatalf("typed field filter should use an indexed field_values lookup, query plan:\n%s", planText)
	}
}

func TestSQLObjectFieldFiltersMatchTypedValuesAndDefaults(t *testing.T) {
	f := newImprovementFixture(t)
	createField := func(name, kind string, options, defaultValue any) string {
		t.Helper()
		return f.create(t, "/api/object-types/"+f.typ+"/fields", map[string]any{"name": name, "field_type": kind, "options": options, "default_value": defaultValue})
	}
	score := createField("Default score", "decimal", map[string]any{}, 5)
	f.u.expect("PATCH", "/api/objects/"+f.object+"/values", map[string]any{"values": map[string]any{score: 10}}, 200)
	note := createField("Note", "text", map[string]any{}, nil)
	choice := createField("Rank", "select", map[string]any{"choices": []string{"bronze", "silver", "gold"}}, nil)
	checked := createField("Checked", "boolean", map[string]any{}, nil)
	date := createField("Date", "date", map[string]any{}, nil)
	progress := createField("Progress", "progress", map[string]any{}, nil)
	makeObject := func(name string, values map[string]any) string {
		t.Helper()
		return f.create(t, "/api/games/"+f.game+"/objects", map[string]any{"name": name, "object_type_id": f.typ, "values": values})
	}
	alpha := makeObject("Alpha", map[string]any{score: -2, note: "Crystal cave", choice: "bronze", checked: true, date: "2025-04-03", progress: map[string]any{"current": 1, "max": 4}})
	beta := makeObject("Beta", map[string]any{score: 20, note: "Castle", choice: "gold", checked: false, date: "2026-02-10", progress: map[string]any{"current": 4, "max": 4}})
	gamma := makeObject("Gamma", map[string]any{score: 100, note: "Crystal bridge", choice: "silver", checked: true, date: "2026-04-10", progress: map[string]any{"current": 3, "max": 4}})
	defaulted := makeObject("Defaulted", map[string]any{})

	query := func(mode string, conditions []any, sort string) map[string]any {
		t.Helper()
		raw, err := json.Marshal(map[string]any{"mode": mode, "conditions": conditions})
		if err != nil {
			t.Fatal(err)
		}
		path := "/api/games/" + f.game + "/objects?type_id=" + url.QueryEscape(f.typ) + "&sort=" + url.QueryEscape(sort) + "&direction=asc&limit=20&filters=" + url.QueryEscape(string(raw))
		return decodeMap(t, f.u.expect("GET", path, nil, 200))
	}
	itemsByName := func(result map[string]any) []string {
		t.Helper()
		items := result["items"].([]any)
		names := make([]string, 0, len(items))
		for _, item := range items {
			names = append(names, mapString(item.(map[string]any), "name"))
		}
		return names
	}
	filtered := query("all", []any{map[string]any{"field_id": score, "operator": "gte", "value": 5}}, score)
	if filtered["total"] != float64(4) || fmt.Sprint(itemsByName(filtered)) != "[Defaulted Example Beta Gamma]" {
		t.Fatalf("numeric SQL filtering, numeric ordering, and default values should be global and typed: %#v", filtered)
	}
	filtered = query("all", []any{map[string]any{"field_id": note, "operator": "contains", "value": "CRYSTAL"}}, "name")
	if fmt.Sprint(itemsByName(filtered)) != "[Alpha Gamma]" {
		t.Fatalf("text contains should remain case-insensitive: %#v", filtered)
	}
	filtered = query("any", []any{map[string]any{"field_id": score, "operator": "lt", "value": 0}, map[string]any{"field_id": note, "operator": "contains", "value": "cave"}}, "name")
	if filtered["total"] != float64(1) || itemsByName(filtered)[0] != "Alpha" {
		t.Fatalf("ANY conditions should be applied across the full collection: %#v", filtered)
	}
	filtered = query("all", []any{map[string]any{"field_id": choice, "operator": "one_of", "value": []string{"bronze", "gold"}}}, "name")
	if fmt.Sprint(itemsByName(filtered)) != "[Alpha Beta]" {
		t.Fatalf("select one-of should preserve exact choices: %#v", filtered)
	}
	filtered = query("all", []any{map[string]any{"field_id": checked, "operator": "checked"}}, "name")
	if fmt.Sprint(itemsByName(filtered)) != "[Alpha Gamma]" {
		t.Fatalf("boolean filters should preserve true and false values: %#v", filtered)
	}
	filtered = query("all", []any{map[string]any{"field_id": date, "operator": "after", "value": "2026-01-01"}}, "name")
	if fmt.Sprint(itemsByName(filtered)) != "[Beta Gamma]" {
		t.Fatalf("date filters should use date ordering: %#v", filtered)
	}
	filtered = query("all", []any{map[string]any{"field_id": progress, "operator": "above", "value": 50}}, "name")
	if fmt.Sprint(itemsByName(filtered)) != "[Beta Gamma]" {
		t.Fatalf("progress filters should compare the calculated ratio: %#v", filtered)
	}
	formula := f.create(t, "/api/object-types/"+f.typ+"/fields", map[string]any{"name": "Calculated score", "field_type": "formula", "options": map[string]any{"expression": "@{" + score + "} + 1"}})
	filtered = query("all", []any{map[string]any{"field_id": formula, "operator": "gte", "value": 20}}, formula)
	if fmt.Sprint(itemsByName(filtered)) != "[Beta Gamma]" {
		t.Fatalf("formula-dependent filters and sorting should retain the Go evaluator: %#v", filtered)
	}
	if alpha == beta || beta == gamma || gamma == defaulted {
		t.Fatal("fixture object IDs should be distinct")
	}
}

func TestSectionProgressDirectRecursiveDeduplicationCyclesAndArchivedChildren(t *testing.T) {
	f := newImprovementFixture(t)
	relation := f.create(t, "/api/object-types/"+f.typ+"/fields", map[string]any{"name": "Children", "field_type": "references", "options": map[string]any{"allowed_type_id": f.typ}})
	progress := f.create(t, "/api/object-types/"+f.typ+"/fields", map[string]any{"name": "Progress", "field_type": "progress"})
	create := func(name string, refs []string, current, maximum any) string {
		t.Helper()
		values := map[string]any{}
		if refs != nil {
			values[relation] = refs
		}
		if current != nil {
			values[progress] = map[string]any{"current": current, "max": maximum}
		}
		return f.create(t, "/api/games/"+f.game+"/objects", map[string]any{"name": name, "object_type_id": f.typ, "values": values})
	}
	leaf := create("Leaf", nil, 3, 5)
	branchA := create("Branch A", []string{leaf}, 2, 4)
	branchB := create("Branch B", []string{leaf}, 1, 4)
	root := create("Root", []string{branchA, branchB}, nil, nil)
	deepLeaf := create("Deep leaf", nil, 7, 10)
	deep4 := create("Depth four", []string{deepLeaf}, 1, 2)
	deep3 := create("Depth three", []string{deep4}, 1, 2)
	deep2 := create("Depth two", []string{deep3}, 1, 2)
	deep1 := create("Depth one", []string{deep2}, 1, 2)
	deepRoot := create("Deep root", []string{deep1}, nil, nil)
	emptyRoot := create("Empty root", []string{}, nil, nil)
	section := f.section(t, "Progress", nil)
	f.u.expect("POST", "/api/sections/"+section+"/placements", map[string]any{"object_id": root}, 204)
	f.u.expect("POST", "/api/sections/"+section+"/placements", map[string]any{"object_id": deepRoot}, 204)
	f.u.expect("POST", "/api/sections/"+section+"/placements", map[string]any{"object_id": emptyRoot}, 204)

	read := func(depth, objectID string) map[string]any {
		t.Helper()
		config := map[string]any{"progress_mode": "children", "progress_depth": depth, "progress_relationship_field_id": relation, "progress_child_field_id": progress}
		f.u.expect("PATCH", "/api/sections/"+section, map[string]any{"display_style": "progress", "display_config": config}, 200)
		var found map[string]any
		var lastPage map[string]any
		for offset := 0; offset < 3 && found == nil; offset++ {
			lastPage = decodeMap(t, f.u.expect("GET", "/api/games/"+f.game+"/objects?section_id="+section+"&limit=1&offset="+strconv.Itoa(offset), nil, 200))
			for _, item := range lastPage["items"].([]any) {
				if item.(map[string]any)["id"] == objectID {
					found = item.(map[string]any)
					break
				}
			}
		}
		if found == nil {
			t.Fatalf("section root query returned unexpected page: %#v", lastPage)
		}
		return found["section_progress"].(map[string]any)
	}
	direct := read("direct", root)
	if direct["current"] != float64(3) || direct["maximum"] != float64(8) || direct["contributing_objects"] != float64(2) || direct["status"] != "complete" {
		t.Fatalf("direct mode should sum immediate children: %#v", direct)
	}
	recursive := read("recursive", root)
	if recursive["current"] != float64(3) || recursive["maximum"] != float64(5) || recursive["percentage"] != float64(60) || recursive["contributing_objects"] != float64(1) || recursive["status"] != "complete" {
		t.Fatalf("recursive mode should count a shared leaf once: %#v", recursive)
	}
	deep := read("recursive", deepRoot)
	if deep["current"] != float64(7) || deep["maximum"] != float64(10) || deep["percentage"] != float64(70) || deep["status"] != "complete" {
		t.Fatalf("recursive traversal should reach a five-level leaf: %#v", deep)
	}
	empty := read("recursive", emptyRoot)
	if empty["status"] != "incomplete" || empty["percentage"] != float64(0) || empty["maximum"] != float64(0) {
		t.Fatalf("empty or missing progress must not appear complete: %#v", empty)
	}
	f.u.expect("POST", "/api/objects/"+branchA+"/archive", map[string]any{}, 204)
	partial := read("recursive", root)
	if partial["status"] != "partial" || len(partial["warnings"].([]any)) == 0 || partial["current"] != float64(3) {
		t.Fatalf("archived children should be excluded and reported while retaining valid leaves: %#v", partial)
	}
	f.u.expect("POST", "/api/objects/"+branchA+"/restore", map[string]any{}, 204)
	f.u.expect("PATCH", "/api/objects/"+branchB+"/values", map[string]any{"values": map[string]any{relation: []string{leaf, root}}}, 200)
	cycle := read("recursive", root)
	if cycle["status"] != "error" || !strings.Contains(strings.Join(anyStrings(cycle["warnings"].([]any)), " "), "Circular references detected") {
		t.Fatalf("recursive cycles should report the object path: %#v", cycle)
	}
}

func TestGoalQueriesFilterSortAndPaginateBeforeReturningResults(t *testing.T) {
	f := newImprovementFixture(t)
	create := func(title string, completed bool, kind string) string {
		t.Helper()
		body := map[string]any{"title": title, "goal_type": kind}
		if kind == "manual" {
			body["completed"] = completed
		} else if kind == "numeric" {
			body["target"] = 100
		}
		return f.create(t, "/api/games/"+f.game+"/goals", body)
	}
	charlie := create("Quest Charlie", false, "manual")
	create("Quest Alpha", true, "manual")
	create("Other", false, "numeric")
	page := decodeMap(t, f.u.expect("GET", "/api/games/"+f.game+"/goals?search=QUEST&type=manual&status=active&sort=title&direction=asc&limit=1&offset=0", nil, 200))
	items := page["items"].([]any)
	if page["total"] != float64(1) || page["game_total"] != float64(3) || len(items) != 1 || items[0].(map[string]any)["title"] != "Quest Charlie" || page["has_more"] != false {
		t.Fatalf("goal filters, sorting, and global counts should apply before paging: %#v", page)
	}
	section := f.section(t, "Only Charlie", nil)
	f.u.expect("POST", "/api/sections/"+section+"/goals", map[string]any{"goal_id": charlie}, 204)
	placed := decodeMap(t, f.u.expect("GET", "/api/games/"+f.game+"/goals?section_id="+section, nil, 200))
	if placed["total"] != float64(1) || placed["items"].([]any)[0].(map[string]any)["id"] != charlie {
		t.Fatalf("section goal listing should return its shared placement: %#v", placed)
	}
	available := decodeMap(t, f.u.expect("GET", "/api/games/"+f.game+"/goals?exclude_section_id="+section+"&limit=20", nil, 200))
	if available["total"] != float64(2) {
		t.Fatalf("section picker should exclude goals already placed there: %#v", available)
	}
	checklist := f.create(t, "/api/games/"+f.game+"/goals", map[string]any{"title": "Checklist", "goal_type": "checklist", "task_details": []any{map[string]any{"title": "First", "completed": true}, map[string]any{"title": "Second", "completed": false}}})
	checklistSection := f.section(t, "Checklist section", nil)
	f.u.expect("POST", "/api/sections/"+checklistSection+"/goals", map[string]any{"goal_id": checklist}, 204)
	checklistPage := decodeMap(t, f.u.expect("GET", "/api/games/"+f.game+"/goals?type=checklist&sort=title&direction=asc&limit=1", nil, 200))
	checklistItem := checklistPage["items"].([]any)[0].(map[string]any)
	if checklistItem["id"] != checklist || checklistItem["progress"] != float64(50) || len(checklistItem["tasks"].([]any)) != 2 || checklistItem["section_ids"].([]any)[0] != checklistSection {
		t.Fatalf("paged checklist details should preserve task progress and placements: %#v", checklistItem)
	}
	detail := decodeMap(t, f.u.expect("GET", "/api/goals/"+charlie, nil, 200))
	if detail["id"] != charlie || detail["title"] != "Quest Charlie" {
		t.Fatalf("single-goal endpoint should load full editor data: %#v", detail)
	}
	all := decodeMap(t, f.u.expect("GET", "/api/games/"+f.game+"/goals", nil, 200))
	if len(all["items"].([]any)) != 4 {
		t.Fatalf("legacy unpaged goal listing should remain complete: %#v", all)
	}
}

func anyStrings(values []any) []string {
	result := make([]string, 0, len(values))
	for _, value := range values {
		if text, ok := value.(string); ok {
			result = append(result, text)
		}
	}
	return result
}

func TestFormulaPreviewPreservesRelationshipAggregations(t *testing.T) {
	f := newImprovementFixture(t)
	relation := f.create(t, "/api/object-types/"+f.typ+"/fields", map[string]any{"name": "Linked items", "field_type": "references", "options": map[string]any{"allowed_type_id": f.typ}})
	child := func(name string, value int) string {
		return f.create(t, "/api/games/"+f.game+"/objects", map[string]any{"name": name, "object_type_id": f.typ, "values": map[string]any{f.number: value}})
	}
	a, b := child("A", 3), child("B", 10)
	f.u.expect("PATCH", "/api/objects/"+f.object+"/values", map[string]any{"values": map[string]any{relation: []string{a, b}}}, 200)
	for _, example := range []struct {
		op   string
		want float64
	}{{"sum", 23}, {"avg", 16.5}, {"min", 13}, {"max", 20}, {"count", 12}} {
		t.Run(example.op, func(t *testing.T) {
			result := decodeMap(t, f.u.expect("POST", "/api/object-types/"+f.typ+"/formula-preview", map[string]any{"object_id": f.object, "options": map[string]any{"expression": "@{related} + @{" + f.number + "}", "aggregation": map[string]any{"relationship_field_id": relation, "field_id": f.number, "operation": example.op}}}, 200))
			if result["result"] != example.want {
				t.Fatalf("%s = %v, want %v", example.op, result, example.want)
			}
		})
	}
}

func TestSavedFiltersSupportAllCustomFieldFamilies(t *testing.T) {
	f := newImprovementFixture(t)
	sectionID := f.section(t, "Filters", nil)
	cases := []struct {
		kind, op     string
		options      any
		value, upper any
	}{
		{"text", "starts_with", nil, "Pixel", nil},
		{"long_text", "not_contains", nil, "Other", nil},
		{"select", "one_of", map[string]any{"choices": []string{"A", "B"}}, []string{"A", "B"}, nil},
		{"boolean", "unchecked", nil, nil, nil},
		{"date", "between", nil, "2026-01-01", "2026-11-04"},
		{"progress", "above", nil, 75, nil},
		{"references", "eq", map[string]any{"allowed_type_id": f.typ}, f.object, nil},
	}
	for _, example := range cases {
		t.Run(example.kind, func(t *testing.T) {
			field := f.create(t, "/api/object-types/"+f.typ+"/fields", map[string]any{"name": example.kind, "field_type": example.kind, "options": example.options})
			view := map[string]any{"search": "", "type_id": f.typ, "mode": "any", "sort": "name", "direction": "asc", "conditions": []any{map[string]any{"field_id": field, "operator": example.op, "value": example.value, "value_to": example.upper}}}
			f.u.expect("PATCH", "/api/sections/"+sectionID, map[string]any{"display_config": map[string]any{"object_view": view}}, 200)
		})
	}
}
