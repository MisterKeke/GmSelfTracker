package httpapi

import (
	"net/http"
	"net/http/httptest"
	"strings"
	"sync"
	"sync/atomic"
	"testing"
)

func TestAuthenticationOriginAndContentType(t *testing.T) {
	e := newTestEnvironment(t)
	for _, path := range []string{"/api/auth/login", "/api/auth/register"} {
		r := httptest.NewRequest("POST", "http://localhost"+path, strings.NewReader(`{}`))
		r.Header.Set("Content-Type", "application/json")
		r.Header.Set("Origin", "https://attacker.example")
		w := httptest.NewRecorder()
		e.handler.ServeHTTP(w, r)
		if w.Code != http.StatusForbidden {
			t.Fatalf("cross-origin %s: %d", path, w.Code)
		}
		r = httptest.NewRequest("POST", "http://localhost"+path, strings.NewReader(`{}`))
		r.Header.Set("Content-Type", "text/plain")
		w = httptest.NewRecorder()
		e.handler.ServeHTTP(w, r)
		if w.Code != http.StatusUnsupportedMediaType {
			t.Fatalf("plain text %s: %d", path, w.Code)
		}
		if w.Header().Get("Content-Security-Policy") == "" || w.Header().Get("Cache-Control") != "no-store" {
			t.Fatal("missing response security policy")
		}
	}
}

func TestAuthenticationLimiterReservesConcurrentAttempts(t *testing.T) {
	limiter := newLoginLimiter()
	var allowed atomic.Int32
	var wg sync.WaitGroup
	for i := 0; i < 100; i++ {
		wg.Add(1)
		go func() {
			defer wg.Done()
			if limiter.take("127.0.0.1") {
				allowed.Add(1)
			}
		}()
	}
	wg.Wait()
	if allowed.Load() != 10 {
		t.Fatalf("reserved %d attempts, want 10", allowed.Load())
	}
}

func TestImageURLPolicy(t *testing.T) {
	for _, value := range []string{"javascript:alert(1)", "data:image/svg+xml,x", "//evil.example/x", "/api/auth/logout", "https://user:pass@example.com/x"} {
		if validImageURL(value) {
			t.Fatalf("accepted unsafe image: %s", value)
		}
	}
}

func TestLocalRulesAggregationPlacementsAndTypeArchive(t *testing.T) {
	e := newTestEnvironment(t)
	u := e.client()
	u.register("local_features")
	create := func(path string, body map[string]any) map[string]any {
		return decodeMap(t, u.expect("POST", path, body, http.StatusCreated))
	}
	g := mapString(create("/api/games", map[string]any{"name": "Rules"}), "id")
	typ := mapString(create("/api/games/"+g+"/object-types", map[string]any{"name": "Record"}), "id")
	field := func(name, kind string, options map[string]any) string {
		return mapString(create("/api/object-types/"+typ+"/fields", map[string]any{"name": name, "field_type": kind, "options": options}), "id")
	}
	n := field("Count", "integer", map[string]any{})
	text := field("State", "text", map[string]any{})
	date := field("Date", "date", map[string]any{})
	links := field("Children", "references", map[string]any{"allowed_type_id": typ})
	sum := field("Child sum", "formula", map[string]any{"expression": "@{related} * 2", "aggregation": map[string]any{"relationship_field_id": links, "field_id": n, "operation": "sum"}})
	child := mapString(create("/api/games/"+g+"/objects", map[string]any{"name": "Child", "object_type_id": typ, "values": map[string]any{n: 4}}), "id")
	parent := create("/api/games/"+g+"/objects", map[string]any{"name": "Parent", "object_type_id": typ, "values": map[string]any{n: 2, text: "ready", date: "2026-11-04", links: []string{child}}})
	parentID := mapString(parent, "id")
	if parent["values"].(map[string]any)[sum] != float64(8) {
		t.Fatalf("wrong aggregate: %#v", parent)
	}
	goal := create("/api/games/"+g+"/goals", map[string]any{"title": "Ready", "goal_type": "field_target", "mode": "all", "conditions": []map[string]any{
		{"object_id": parentID, "field_id": sum, "operator": "gte", "value": 8},
		{"object_id": parentID, "field_id": text, "operator": "eq", "value": "ready"},
		{"object_id": parentID, "field_id": date, "operator": "gte", "value": "2026-11-01"},
	}})
	if goal["completed"] != true {
		t.Fatalf("rules should match: %#v", goal)
	}
	section := mapString(create("/api/games/"+g+"/sections", map[string]any{"name": "Progress", "display_style": "progress", "display_config": map[string]any{"progress_mode": "calculated", "progress_current_field_ids": []string{n, sum}, "progress_maximum": 20}}), "id")
	goalID := mapString(goal, "id")
	u.expect("POST", "/api/sections/"+section+"/goals", map[string]any{"goal_id": goalID}, http.StatusNoContent)
	goals := decodeMap(t, u.expect("GET", "/api/games/"+g+"/goals", nil, http.StatusOK))["items"].([]any)
	if len(goals[0].(map[string]any)["section_ids"].([]any)) != 1 {
		t.Fatal("goal placement missing")
	}
	otherType := mapString(create("/api/games/"+g+"/object-types", map[string]any{"name": "Other"}), "id")
	otherField := mapString(create("/api/object-types/"+otherType+"/fields", map[string]any{"name": "Other count", "field_type": "integer"}), "id")
	u.expect("POST", "/api/games/"+g+"/goals", map[string]any{"title": "Invalid", "goal_type": "field_target", "object_id": parentID, "field_id": otherField, "target": 1}, http.StatusBadRequest)
	u.expect("POST", "/api/object-types/"+typ+"/archive", map[string]any{}, http.StatusNoContent)
	objects := decodeMap(t, u.expect("GET", "/api/games/"+g+"/objects", nil, http.StatusOK))["items"].([]any)
	if len(objects) != 0 {
		t.Fatal("archived type objects should be hidden")
	}
	u.expect("POST", "/api/object-types/"+typ+"/restore", map[string]any{}, http.StatusNoContent)
	u.expect("GET", "/api/objects/"+parentID, nil, http.StatusOK)
	u.expect("POST", "/api/fields/"+links+"/archive", map[string]any{}, http.StatusConflict)
	u.expect("DELETE", "/api/sections/"+section+"/goals/"+goalID, nil, http.StatusNoContent)
}
