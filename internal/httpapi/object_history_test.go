package httpapi

import (
	"encoding/json"
	"net/url"
	"testing"
)

func TestObjectHistoryRecordsAtomicChangesAndSupportsFilters(t *testing.T) {
	f := newImprovementFixture(t)
	historyPath := "/api/objects/" + f.object + "/history"
	read := func(path string) map[string]any {
		t.Helper()
		return decodeMap(t, f.u.expect("GET", path, nil, 200))
	}
	decodeItems := func(data map[string]any) []objectHistoryItem {
		t.Helper()
		raw, err := json.Marshal(data["items"])
		if err != nil {
			t.Fatal(err)
		}
		var items []objectHistoryItem
		if err := json.Unmarshal(raw, &items); err != nil {
			t.Fatal(err)
		}
		return items
	}

	created := read(historyPath)
	if created["total"] != float64(1) {
		t.Fatalf("created history total=%v", created["total"])
	}
	items := decodeItems(created)
	if items[0].EventType != "created" || items[0].Source != "web" || len(items[0].Changes) != 1 {
		t.Fatalf("creation history: %+v", items)
	}
	if items[0].Changes[0].FieldID != f.number || items[0].Changes[0].FieldName != "Custom value" || string(items[0].Changes[0].After) != "10" {
		t.Fatalf("creation value snapshot: %+v", items[0].Changes[0])
	}

	f.u.expect("PATCH", "/api/objects/"+f.object, map[string]any{
		"name": "Renamed", "values": map[string]any{f.number: 20},
	}, 200)
	items = decodeItems(read(historyPath))
	if len(items) != 2 || items[0].EventType != "updated" || len(items[0].Changes) != 2 {
		t.Fatalf("object update history: %+v", items)
	}
	fieldChange := items[0].Changes[1]
	if fieldChange.FieldID != f.number || fieldChange.FieldName != "Custom value" || string(fieldChange.Before) != "10" || string(fieldChange.After) != "20" {
		t.Fatalf("field update snapshot: %+v", fieldChange)
	}

	f.u.expect("PATCH", "/api/fields/"+f.number, map[string]any{"name": "Renamed field", "field_type": "decimal"}, 200)
	filtered := read(historyPath + "?field_id=" + url.QueryEscape(f.number))
	if filtered["total"] != float64(2) {
		t.Fatalf("field filter total=%v", filtered["total"])
	}
	items = decodeItems(filtered)
	if items[0].Changes[1].FieldName != "Custom value" {
		t.Fatalf("history should preserve the historical field label: %+v", items[0].Changes)
	}

	beforeFailure := filtered["total"]
	f.u.expect("PATCH", "/api/objects/"+f.object+"/values", map[string]any{"values": map[string]any{f.number: "not a number"}}, 400)
	afterFailure := read(historyPath + "?field_id=" + url.QueryEscape(f.number))
	if afterFailure["total"] != beforeFailure {
		t.Fatal("a rejected value update created a history event")
	}

	f.u.expect("POST", "/api/objects/"+f.object+"/archive", map[string]any{}, 204)
	f.u.expect("POST", "/api/objects/"+f.object+"/restore", map[string]any{}, 204)
	items = decodeItems(read(historyPath))
	if len(items) != 4 || items[0].EventType != "restored" || items[1].EventType != "archived" {
		t.Fatalf("archive/restore history order: %+v", items)
	}

	outsider := f.e.client()
	outsider.register("history-outsider")
	outsider.expect("GET", historyPath, nil, 404)
	f.u.expect("GET", historyPath+"?event_type=deleted", nil, 400)
	f.u.expect("GET", historyPath+"?since=not-a-date", nil, 400)
}

func TestObjectHistoryPaginatesEvents(t *testing.T) {
	f := newImprovementFixture(t)
	for _, name := range []string{"One", "Two", "Three"} {
		f.u.expect("PATCH", "/api/objects/"+f.object, map[string]any{"name": name}, 200)
	}
	page := decodeMap(t, f.u.expect("GET", "/api/objects/"+f.object+"/history?limit=2&offset=1", nil, 200))
	var items []objectHistoryItem
	raw, _ := json.Marshal(page["items"])
	if err := json.Unmarshal(raw, &items); err != nil {
		t.Fatal(err)
	}
	if page["total"] != float64(4) || page["has_more"] != true || len(items) != 2 {
		t.Fatalf("history page metadata: %#v", page)
	}
}
