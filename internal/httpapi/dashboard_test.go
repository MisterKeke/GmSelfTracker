package httpapi

import (
	"bytes"
	"encoding/json"
	"image"
	"image/png"
	"net/http"
	"testing"
)

func TestGameDashboardDefaultSaveAndOwnership(t *testing.T) {
	f := newImprovementFixture(t)
	path := "/api/games/" + f.game + "/dashboard"
	initial := decodeMap(t, f.u.expect("GET", path, nil, http.StatusOK))
	if initial["is_default"] != true {
		t.Fatalf("new dashboard should be marked default: %#v", initial)
	}
	var defaultLayout dashboardLayout
	if err := json.Unmarshal(mustJSON(t, initial["layout"]), &defaultLayout); err != nil || len(defaultLayout.Widgets) != 3 {
		t.Fatalf("default dashboard should have the overview widgets: %#v, err=%v", initial["layout"], err)
	}

	layout := map[string]any{"version": 1, "widgets": []any{
		map[string]any{"id": "notes-1", "type": "notes", "title": "  Session Notes  ", "size": "small", "config": map[string]any{"text": "Keep this plain text."}},
		map[string]any{"id": "objects-1", "type": "object_collection", "title": "My objects", "size": "large", "config": map[string]any{"type_id": f.typ, "limit": 10, "sort": "name", "direction": "asc"}},
	}}
	f.u.expect("PUT", path, map[string]any{"layout": layout}, http.StatusOK)
	saved := decodeMap(t, f.u.expect("GET", path, nil, http.StatusOK))
	if saved["is_default"] != false {
		t.Fatalf("saved dashboard should not be default: %#v", saved)
	}
	var got dashboardLayout
	if err := json.Unmarshal(mustJSON(t, saved["layout"]), &got); err != nil {
		t.Fatal(err)
	}
	if len(got.Widgets) != 2 || got.Widgets[0].Title != "Session Notes" {
		t.Fatalf("saved layout was not normalized and persisted: %#v", got)
	}
	f.u.expect("POST", "/api/object-types/"+f.typ+"/archive", map[string]any{}, http.StatusNoContent)
	stillConfigured := decodeMap(t, f.u.expect("GET", path, nil, http.StatusOK))
	var retained dashboardLayout
	if err := json.Unmarshal(mustJSON(t, stillConfigured["layout"]), &retained); err != nil || len(retained.Widgets) != 2 || retained.Widgets[1].Config["type_id"] == nil {
		t.Fatalf("archiving a widget source must not erase the saved layout: %#v, err=%v", stillConfigured, err)
	}
	secondGame := f.create(t, "/api/games", map[string]any{"name": "Separate dashboard"})
	secondLayout := map[string]any{"version": 1, "widgets": []any{map[string]any{"id": "second-notes", "type": "notes", "title": "Other game", "size": "small", "config": map[string]any{"text": "Independent"}}}}
	f.u.expect("PUT", "/api/games/"+secondGame+"/dashboard", map[string]any{"layout": secondLayout}, http.StatusOK)
	secondSaved := decodeMap(t, f.u.expect("GET", "/api/games/"+secondGame+"/dashboard", nil, http.StatusOK))
	var independent dashboardLayout
	if secondSaved["is_default"] != false || json.Unmarshal(mustJSON(t, secondSaved["layout"]), &independent) != nil || len(independent.Widgets) != 1 || independent.Widgets[0].Title != "Other game" {
		t.Fatalf("dashboard layouts should remain game-scoped: %#v", secondSaved)
	}

	other := f.e.client()
	other.register("dashboard_outsider")
	other.expect("GET", path, nil, http.StatusNotFound)
	other.expect("PUT", path, map[string]any{"layout": layout}, http.StatusNotFound)
}

func TestGameDashboardRejectsInvalidLayoutAndForeignSources(t *testing.T) {
	f := newImprovementFixture(t)
	otherGame := f.create(t, "/api/games", map[string]any{"name": "Other game"})
	foreignType := f.create(t, "/api/games/"+otherGame+"/object-types", map[string]any{"name": "Foreign type"})
	path := "/api/games/" + f.game + "/dashboard"
	validWidget := func(config map[string]any) map[string]any {
		return map[string]any{"id": "widget", "type": "object_collection", "title": "Objects", "size": "medium", "config": config}
	}
	for name, layout := range map[string]any{
		"duplicate widget IDs": map[string]any{"version": 1, "widgets": []any{validWidget(map[string]any{"type_id": f.typ}), validWidget(map[string]any{"type_id": f.typ})}},
		"foreign type":         map[string]any{"version": 1, "widgets": []any{validWidget(map[string]any{"type_id": foreignType})}},
		"unsupported sort":     map[string]any{"version": 1, "widgets": []any{validWidget(map[string]any{"type_id": f.typ, "sort": "secret_column"})}},
		"too many widgets":     map[string]any{"version": 1, "widgets": makeDashboardWidgets(21, f.typ)},
	} {
		t.Run(name, func(t *testing.T) {
			f.u.expect("PUT", path, map[string]any{"layout": layout}, http.StatusBadRequest)
		})
	}
}

func TestDashboardImageReferenceProtectsUploadedAsset(t *testing.T) {
	f := newImprovementFixture(t)
	var pngData bytes.Buffer
	if err := png.Encode(&pngData, image.NewRGBA(image.Rect(0, 0, 1, 1))); err != nil {
		t.Fatal(err)
	}
	status, data := f.u.multipart("/api/assets", pngData.Bytes(), "dashboard.png")
	if status != http.StatusCreated {
		t.Fatalf("upload status=%d body=%s", status, data)
	}
	assetID := mapString(decodeMap(t, data), "id")
	path := "/api/games/" + f.game + "/dashboard"
	imageWidget := map[string]any{"id": "hero", "type": "image", "title": "Cover", "size": "large", "config": map[string]any{"asset_id": assetID}}
	f.u.expect("PUT", path, map[string]any{"layout": map[string]any{"version": 1, "widgets": []any{imageWidget}}}, http.StatusOK)
	f.u.expect("DELETE", "/api/assets/"+assetID, nil, http.StatusConflict)
	f.u.expect("PUT", path, map[string]any{"layout": map[string]any{"version": 1, "widgets": []any{}}}, http.StatusOK)
	f.u.expect("DELETE", "/api/assets/"+assetID, nil, http.StatusNoContent)
}

func makeDashboardWidgets(count int, typeID string) []any {
	widgets := make([]any, count)
	for i := range widgets {
		widgets[i] = map[string]any{"id": "widget-" + string(rune('a'+i)), "type": "object_collection", "title": "Objects", "size": "medium", "config": map[string]any{"type_id": typeID}}
	}
	return widgets
}

func mustJSON(t *testing.T, value any) []byte {
	t.Helper()
	data, err := json.Marshal(value)
	if err != nil {
		t.Fatal(err)
	}
	return data
}
