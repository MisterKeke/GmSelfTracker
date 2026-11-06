package httpapi

import "testing"

func TestGoalDependenciesReadinessCycleProtectionAndDeletion(t *testing.T) {
	f := newImprovementFixture(t)
	createGoal := func(title string, completed bool) string {
		return f.create(t, "/api/games/"+f.game+"/goals", map[string]any{"title": title, "goal_type": "manual", "completed": completed})
	}
	prerequisite := createGoal("Prerequisite", false)
	second := createGoal("Second prerequisite", true)
	dependent := createGoal("Dependent", false)
	path := "/api/goals/" + dependent + "/dependencies"
	result := decodeMap(t, f.u.expect("PUT", path, map[string]any{"depends_on_ids": []string{prerequisite, second}}, 200))
	blockedBy := result["blocked_by"].([]any)
	if result["ready"] != false || len(blockedBy) != 1 || blockedBy[0] != "Prerequisite" {
		t.Fatalf("dependent goal readiness=%#v", result)
	}
	f.u.expect("PATCH", "/api/goals/"+dependent, map[string]any{"completed": true}, 409)
	f.u.expect("PATCH", "/api/goals/"+prerequisite, map[string]any{"completed": true}, 200)
	ready := decodeMap(t, f.u.expect("GET", path, nil, 200))
	if ready["ready"] != true || len(ready["blocked_by"].([]any)) != 0 {
		t.Fatalf("completion should unlock dependent goal: %#v", ready)
	}
	f.u.expect("PATCH", "/api/goals/"+dependent, map[string]any{"completed": true}, 200)
	// Adding prerequisite -> dependent would close the existing dependent -> prerequisite chain.
	f.u.expect("PUT", "/api/goals/"+prerequisite+"/dependencies", map[string]any{"depends_on_ids": []string{dependent}}, 400)
	f.u.expect("DELETE", "/api/goals/"+prerequisite, nil, 204)
	remaining := decodeMap(t, f.u.expect("GET", path, nil, 200))
	if remaining["ready"] != true || len(remaining["items"].([]any)) != 1 {
		t.Fatalf("deleted prerequisite should be removed without blocking: %#v", remaining)
	}
}

func TestGoalDependenciesRejectCrossGameAndSelfLinks(t *testing.T) {
	f := newImprovementFixture(t)
	goalID := f.create(t, "/api/games/"+f.game+"/goals", map[string]any{"title": "Local", "goal_type": "manual"})
	otherGame := f.create(t, "/api/games", map[string]any{"name": "Another game"})
	otherGoal := f.create(t, "/api/games/"+otherGame+"/goals", map[string]any{"title": "Foreign game", "goal_type": "manual"})
	path := "/api/goals/" + goalID + "/dependencies"
	f.u.expect("PUT", path, map[string]any{"depends_on_ids": []string{goalID}}, 400)
	f.u.expect("PUT", path, map[string]any{"depends_on_ids": []string{otherGoal}}, 400)
	otherUser := f.e.client()
	otherUser.register("dependency_foreign")
	otherUser.expect("PUT", path, map[string]any{"depends_on_ids": []string{}}, 404)
}

func TestChecklistCannotCompleteBeforePrerequisites(t *testing.T) {
	f := newImprovementFixture(t)
	prerequisite := f.create(t, "/api/games/"+f.game+"/goals", map[string]any{"title": "Unlock", "goal_type": "manual"})
	checklist := f.create(t, "/api/games/"+f.game+"/goals", map[string]any{"title": "Checklist", "goal_type": "checklist", "tasks": []string{"Finish this"}})
	f.u.expect("PUT", "/api/goals/"+checklist+"/dependencies", map[string]any{"depends_on_ids": []string{prerequisite}}, 200)
	var taskID string
	if err := f.e.db.QueryRow(`SELECT id FROM goal_tasks WHERE goal_id=?`, checklist).Scan(&taskID); err != nil {
		t.Fatal(err)
	}
	f.u.expect("PATCH", "/api/goal-tasks/"+taskID, map[string]any{"completed": true}, 409)
	f.u.expect("PATCH", "/api/goals/"+prerequisite, map[string]any{"completed": true}, 200)
	f.u.expect("PATCH", "/api/goal-tasks/"+taskID, map[string]any{"completed": true}, 200)
}
