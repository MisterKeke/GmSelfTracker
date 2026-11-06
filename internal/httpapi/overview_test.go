package httpapi

import (
	"fmt"
	"net/http"
	"sort"
	"testing"
)

func TestGameOverviewUsesRecentGameWideObjectsAndOwnerScope(t *testing.T) {
	e := newTestEnvironment(t)
	u := e.client()
	u.register("overview_owner")
	game := decodeMap(t, u.expect("POST", "/api/games", map[string]any{"name": "Overview Game"}, http.StatusCreated))
	gameID := mapString(game, "id")
	typeDef := decodeMap(t, u.expect("POST", "/api/games/"+gameID+"/object-types", map[string]any{"name": "Entry"}, http.StatusCreated))
	typeID := mapString(typeDef, "id")
	ids := make([]string, 8)
	names := make([]string, 8)
	for i := range ids {
		names[i] = fmt.Sprintf("Entry %d", i)
		item := decodeMap(t, u.expect("POST", "/api/games/"+gameID+"/objects", map[string]any{"object_type_id": typeID, "name": names[i]}, http.StatusCreated))
		ids[i] = mapString(item, "id")
		stamp := fmt.Sprintf("2026-01-%02dT00:00:00Z", i+1)
		if i == 5 || i == 6 {
			stamp = "2026-02-01T00:00:00Z"
		}
		if _, err := e.db.Exec(`UPDATE objects SET updated_at=? WHERE id=?`, stamp, ids[i]); err != nil {
			t.Fatal(err)
		}
	}
	u.expect("POST", "/api/objects/"+ids[7]+"/archive", map[string]any{}, http.StatusNoContent)
	u.expect("POST", "/api/games/"+gameID+"/sections", map[string]any{"name": "Active Section"}, http.StatusCreated)
	archivedSection := decodeMap(t, u.expect("POST", "/api/games/"+gameID+"/sections", map[string]any{"name": "Archived Section"}, http.StatusCreated))
	u.expect("DELETE", "/api/sections/"+mapString(archivedSection, "id"), nil, http.StatusNoContent)

	result := decodeMap(t, u.expect("GET", "/api/games/"+gameID+"/overview", nil, http.StatusOK))
	if result["object_count"] != float64(7) || result["section_count"] != float64(1) {
		t.Fatalf("overview counts should exclude archived records: %#v", result)
	}
	items, ok := result["recent_objects"].([]any)
	if !ok || len(items) != 6 {
		t.Fatalf("overview should return only six recent objects: %#v", result["recent_objects"])
	}
	tieIDs := []string{ids[5], ids[6]}
	sort.Strings(tieIDs)
	expected := []string{names[5], names[6]}
	if tieIDs[0] == ids[6] {
		expected[0], expected[1] = expected[1], expected[0]
	}
	for _, index := range []int{4, 3, 2, 1} {
		expected = append(expected, names[index])
	}
	for i, raw := range items {
		object, ok := raw.(map[string]any)
		if !ok || object["name"] != expected[i] {
			t.Fatalf("recent objects should be ordered by update time with a stable ID tie-breaker: got %#v want %q", raw, expected[i])
		}
	}
	other := e.client()
	other.register("overview_other")
	other.expect("GET", "/api/games/"+gameID+"/overview", nil, http.StatusNotFound)
}
