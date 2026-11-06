package httpapi

import (
	"encoding/json"
	"net/http"
	"testing"
)

func TestRelationshipGraphHandlesCyclesBranchesAndArchivedNodes(t *testing.T) {
	f := newImprovementFixture(t)
	ref := f.create(t, "/api/object-types/"+f.typ+"/fields", map[string]any{"name": "Links", "field_type": "references", "options": map[string]any{"allowed_type_id": f.typ}})
	second := f.create(t, "/api/games/"+f.game+"/objects", map[string]any{"object_type_id": f.typ, "name": "Second"})
	third := f.create(t, "/api/games/"+f.game+"/objects", map[string]any{"object_type_id": f.typ, "name": "Third"})
	f.u.expect("PATCH", "/api/objects/"+f.object+"/values", map[string]any{"values": map[string]any{ref: []string{second, third}}}, http.StatusOK)
	f.u.expect("PATCH", "/api/objects/"+second+"/values", map[string]any{"values": map[string]any{ref: []string{third}}}, http.StatusOK)
	f.u.expect("PATCH", "/api/objects/"+third+"/values", map[string]any{"values": map[string]any{ref: []string{f.object}}}, http.StatusOK)
	path := "/api/objects/" + f.object + "/graph?depth=4&direction=both"
	var graph relationshipGraph
	if err := json.Unmarshal(f.u.expect("GET", path, nil, http.StatusOK), &graph); err != nil {
		t.Fatal(err)
	}
	if len(graph.Nodes) != 3 || len(graph.Edges) != 4 {
		t.Fatalf("branching cycle graph should deduplicate its three nodes and preserve four directed edges: %#v", graph)
	}
	for _, edge := range graph.Edges {
		if edge.FieldID != ref || edge.FieldName != "Links" {
			t.Fatalf("graph edge should use the real reference field: %#v", edge)
		}
	}
	f.u.expect("POST", "/api/objects/"+third+"/archive", map[string]any{}, http.StatusNoContent)
	if err := json.Unmarshal(f.u.expect("GET", path, nil, http.StatusOK), &graph); err != nil {
		t.Fatal(err)
	}
	archivedSeen := false
	for _, node := range graph.Nodes {
		if node.ID == third {
			archivedSeen = node.Archived
		}
	}
	if !archivedSeen {
		t.Fatal("archived referenced objects should remain visible and be marked archived")
	}
}

func TestRelationshipGraphBoundsAndOwnerScope(t *testing.T) {
	f := newImprovementFixture(t)
	ref := f.create(t, "/api/object-types/"+f.typ+"/fields", map[string]any{"name": "Links", "field_type": "references", "options": map[string]any{"allowed_type_id": f.typ}})
	first := f.create(t, "/api/games/"+f.game+"/objects", map[string]any{"object_type_id": f.typ, "name": "First"})
	second := f.create(t, "/api/games/"+f.game+"/objects", map[string]any{"object_type_id": f.typ, "name": "Second"})
	f.u.expect("PATCH", "/api/objects/"+f.object+"/values", map[string]any{"values": map[string]any{ref: []string{first, second}}}, http.StatusOK)
	var graph relationshipGraph
	if err := json.Unmarshal(f.u.expect("GET", "/api/objects/"+f.object+"/graph?depth=2&max_nodes=2&direction=outbound", nil, http.StatusOK), &graph); err != nil {
		t.Fatal(err)
	}
	if len(graph.Nodes) != 2 || !graph.Truncated {
		t.Fatalf("node bound should be reported: %#v", graph)
	}
	other := f.e.client()
	other.register("graph_outsider")
	other.expect("GET", "/api/objects/"+f.object+"/graph", nil, http.StatusNotFound)
	other.expect("GET", "/api/objects/"+first+"/graph", nil, http.StatusNotFound)
	f.u.expect("GET", "/api/objects/"+f.object+"/graph?direction=sideways", nil, http.StatusBadRequest)
}
