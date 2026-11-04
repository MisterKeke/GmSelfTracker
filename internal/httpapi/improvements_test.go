package httpapi

import (
	"encoding/json"
	"net/http"
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
