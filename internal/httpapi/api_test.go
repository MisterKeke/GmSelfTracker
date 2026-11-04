package httpapi

import (
	"bytes"
	"context"
	"database/sql"
	"encoding/json"
	"image"
	"image/png"
	"io"
	"log/slog"
	"mime/multipart"
	"net/http"
	"net/http/httptest"
	"path/filepath"
	"strings"
	"testing"

	"gametracker/internal/database"
)

type testEnvironment struct {
	t       testing.TB
	db      *sql.DB
	handler http.Handler
}
type testUser struct {
	t       testing.TB
	handler http.Handler
	cookie  *http.Cookie
	csrf    string
}

func newTestEnvironment(t testing.TB) *testEnvironment {
	t.Helper()
	db, err := database.Open(context.Background(), filepath.Join(t.TempDir(), "tracker.db"))
	if err != nil {
		t.Fatal(err)
	}
	handler := New(db, filepath.Join(t.TempDir(), "uploads"), false, slog.New(slog.NewTextHandler(io.Discard, nil)))
	t.Cleanup(func() { db.Close() })
	return &testEnvironment{t: t, db: db, handler: handler}
}

func (e *testEnvironment) client() *testUser {
	return &testUser{t: e.t, handler: e.handler}
}

func (u *testUser) call(method, path string, body any) (int, []byte) {
	var encoded []byte
	var err error
	if body != nil {
		encoded, err = json.Marshal(body)
		if err != nil {
			u.t.Fatal(err)
		}
	}
	req, err := http.NewRequest(method, path, bytes.NewReader(encoded))
	if err != nil {
		u.t.Fatal(err)
	}
	if body != nil {
		req.Header.Set("Content-Type", "application/json")
	}
	if u.csrf != "" {
		req.Header.Set("X-CSRF-Token", u.csrf)
	}
	if u.cookie != nil {
		req.AddCookie(u.cookie)
	}
	response := httptest.NewRecorder()
	u.handler.ServeHTTP(response, req)
	resp := response.Result()
	data, err := io.ReadAll(resp.Body)
	if err != nil {
		u.t.Fatal(err)
	}
	for _, cookie := range resp.Cookies() {
		if cookie.Name == "gametracker_session" && cookie.Value != "" {
			u.cookie = cookie
		}
	}
	if resp.StatusCode == http.StatusCreated || resp.StatusCode == http.StatusOK {
		var session userSession
		if json.Unmarshal(data, &session) == nil && session.CSRF != "" {
			u.csrf = session.CSRF
		}
	}
	return resp.StatusCode, data
}

func (u *testUser) register(username string) {
	u.t.Helper()
	status, data := u.call("POST", "/api/auth/register", map[string]any{"username": username, "password": "safe-password-123", "confirm_password": "safe-password-123"})
	if status != http.StatusCreated {
		u.t.Fatalf("register: status %d: %s", status, data)
	}
}
func (u *testUser) expect(method, path string, body any, want int) []byte {
	u.t.Helper()
	status, data := u.call(method, path, body)
	if status != want {
		u.t.Fatalf("%s %s: status %d, want %d: %s", method, path, status, want, data)
	}
	return data
}
func decodeMap(t testing.TB, data []byte) map[string]any {
	t.Helper()
	var value map[string]any
	if err := json.Unmarshal(data, &value); err != nil {
		t.Fatal(err)
	}
	return value
}
func mapString(value map[string]any, key string) string {
	result, _ := value[key].(string)
	return result
}

func TestAuthenticationAndGameOwnershipIsolation(t *testing.T) {
	e := newTestEnvironment(t)
	first := e.client()
	first.register("first_user")
	csrf := first.csrf
	first.csrf = ""
	first.expect("POST", "/api/games", map[string]any{"name": "Blocked without CSRF"}, http.StatusForbidden)
	first.csrf = csrf
	status, _ := e.client().call("GET", "/api/games", nil)
	if status != http.StatusUnauthorized {
		t.Fatalf("unauthenticated status=%d want 401", status)
	}
	status, data := first.call("POST", "/api/games", map[string]any{"name": "Local Game"})
	if status != http.StatusCreated {
		t.Fatalf("create game: %d %s", status, data)
	}
	gameID := mapString(decodeMap(t, data), "id")
	second := e.client()
	second.register("second_user")
	second.expect("GET", "/api/games/"+gameID, nil, http.StatusNotFound)
	second.expect("POST", "/api/games/"+gameID+"/sections", map[string]any{"name": "Intrusion"}, http.StatusNotFound)
	duplicate := e.client()
	duplicate.expect("POST", "/api/auth/register", map[string]any{"username": "first_user", "password": "safe-password-123", "confirm_password": "safe-password-123"}, http.StatusConflict)
	login := e.client()
	login.expect("POST", "/api/auth/login", map[string]any{"username": "first_user", "password": "safe-password-123"}, http.StatusOK)
	login.expect("POST", "/api/auth/password", map[string]any{"current_password": "safe-password-123", "password": "new-safe-password-456", "confirm_password": "new-safe-password-456"}, http.StatusNoContent)
	bad := e.client()
	bad.expect("POST", "/api/auth/login", map[string]any{"username": "first_user", "password": "wrong-password"}, http.StatusUnauthorized)
	oldPassword := e.client()
	oldPassword.expect("POST", "/api/auth/login", map[string]any{"username": "first_user", "password": "safe-password-123"}, http.StatusUnauthorized)
	newPassword := e.client()
	newPassword.expect("POST", "/api/auth/login", map[string]any{"username": "first_user", "password": "new-safe-password-456"}, http.StatusOK)
}

func TestCustomDataSharedPlacementsGoalsAndFieldLifecycle(t *testing.T) {
	e := newTestEnvironment(t)
	u := e.client()
	u.register("tracker_user")
	game := decodeMap(t, u.expect("POST", "/api/games", map[string]any{"name": "My Game"}, http.StatusCreated))
	gameID := mapString(game, "id")
	typeDef := decodeMap(t, u.expect("POST", "/api/games/"+gameID+"/object-types", map[string]any{"name": "Adventurer", "icon": "⚔"}, http.StatusCreated))
	typeID := mapString(typeDef, "id")
	makeField := func(name, kind string, options any) string {
		data := u.expect("POST", "/api/object-types/"+typeID+"/fields", map[string]any{"name": name, "field_type": kind, "options": options}, http.StatusCreated)
		return mapString(decodeMap(t, data), "id")
	}
	levelID := makeField("Level", "integer", map[string]any{})
	critID := makeField("Crit Rate", "percentage", map[string]any{})
	notesID := makeField("Notes", "text", map[string]any{})
	linkID := makeField("Partner", "reference", map[string]any{})
	formulaID := makeField("Combined", "formula", map[string]any{"expression": "@{" + levelID + "} + 10"})
	rootData := decodeMap(t, u.expect("POST", "/api/games/"+gameID+"/sections", map[string]any{"name": "Collection"}, http.StatusCreated))
	rootID := mapString(rootData, "id")
	childData := decodeMap(t, u.expect("POST", "/api/games/"+gameID+"/sections", map[string]any{"name": "Favorites", "parent_id": rootID}, http.StatusCreated))
	childID := mapString(childData, "id")
	rootItem := decodeMap(t, u.expect("POST", "/api/games/"+gameID+"/objects", map[string]any{"object_type_id": typeID, "name": "One", "values": map[string]any{levelID: 64, critID: 70, notesID: "kept"}}, http.StatusCreated))
	objectID := mapString(rootItem, "id")
	secondObject := decodeMap(t, u.expect("POST", "/api/games/"+gameID+"/objects", map[string]any{"object_type_id": typeID, "name": "Two"}, http.StatusCreated))
	secondObjectID := mapString(secondObject, "id")
	u.expect("PATCH", "/api/objects/"+objectID+"/values", map[string]any{"values": map[string]any{levelID: "not a number"}}, http.StatusBadRequest)
	u.expect("GET", "/api/games/"+gameID+"/object-types", nil, http.StatusOK)
	u.expect("GET", "/api/games/"+gameID+"/objects", nil, http.StatusOK)
	u.expect("POST", "/api/sections/"+rootID+"/placements", map[string]any{"object_id": objectID}, http.StatusNoContent)
	u.expect("POST", "/api/sections/"+childID+"/placements", map[string]any{"object_id": objectID}, http.StatusNoContent)
	// The same object ID is placed in both views; a change to its value is shared.
	placements := decodeMap(t, u.expect("GET", "/api/sections/"+rootID+"/placements", nil, http.StatusOK))["items"].([]any)
	if len(placements) != 1 || mapString(placements[0].(map[string]any), "id") != objectID {
		t.Fatalf("root placement did not preserve shared object: %#v", placements)
	}
	sections := decodeMap(t, u.expect("GET", "/api/objects/"+objectID, nil, http.StatusOK))["section_ids"].([]any)
	if len(sections) != 2 {
		t.Fatalf("object should be in two sections; got %#v", sections)
	}
	u.expect("PATCH", "/api/objects/"+objectID+"/values", map[string]any{"values": map[string]any{levelID: 90, critID: 70}}, http.StatusOK)
	object := decodeMap(t, u.expect("GET", "/api/objects/"+objectID, nil, http.StatusOK))
	values := object["values"].(map[string]any)
	if values[levelID] != float64(90) || values[formulaID] != float64(100) {
		t.Fatalf("updated values or formula incorrect: %#v", values)
	}
	goal := decodeMap(t, u.expect("POST", "/api/games/"+gameID+"/goals", map[string]any{"title": "Reach Crit Rate", "goal_type": "field_target", "object_id": objectID, "field_id": critID, "target": 75}, http.StatusCreated))
	goalID := mapString(goal, "id")
	if goal["completed"] != false {
		t.Fatalf("goal should start incomplete: %#v", goal)
	}
	goalList := decodeMap(t, u.expect("GET", "/api/games/"+gameID+"/goals", nil, http.StatusOK))["items"].([]any)
	if len(goalList) != 1 || goalList[0].(map[string]any)["completed"] != false || goalList[0].(map[string]any)["progress"].(float64) < 93 {
		t.Fatalf("linked goal should reflect its live field value: %#v", goalList)
	}
	u.expect("PATCH", "/api/objects/"+objectID+"/values", map[string]any{"values": map[string]any{critID: 80}}, http.StatusOK)
	goalList = decodeMap(t, u.expect("GET", "/api/games/"+gameID+"/goals", nil, http.StatusOK))["items"].([]any)
	if goalList[0].(map[string]any)["completed"] != true {
		t.Fatalf("linked goal should complete after the field target is reached: %#v", goalList)
	}
	checklist := decodeMap(t, u.expect("POST", "/api/games/"+gameID+"/goals", map[string]any{"title": "Prepare build", "goal_type": "checklist", "tasks": []string{"Gather materials", "Upgrade gear"}}, http.StatusCreated))
	checklistID := mapString(checklist, "id")
	tasks := checklist["tasks"].([]any)
	taskID := mapString(tasks[0].(map[string]any), "id")
	u.expect("PATCH", "/api/goal-tasks/"+taskID, map[string]any{"completed": true}, http.StatusOK)
	goalList = decodeMap(t, u.expect("GET", "/api/games/"+gameID+"/goals", nil, http.StatusOK))["items"].([]any)
	for _, item := range goalList {
		candidate := item.(map[string]any)
		if candidate["id"] == checklistID && candidate["progress"].(float64) != 50 {
			t.Fatalf("checklist progress should be 50%% after one of two tasks: %#v", candidate)
		}
	}
	u.expect("PATCH", "/api/fields/"+levelID, map[string]any{"field_type": "text"}, http.StatusConflict)
	u.expect("PATCH", "/api/fields/"+critID, map[string]any{"name": "Critical Chance"}, http.StatusOK)
	// The stable ID means the linked condition survives a field rename.
	if _, err := e.db.Exec(`UPDATE goals SET field_id=? WHERE id=?`, critID, goalID); err != nil {
		t.Fatal(err)
	}
	deps := decodeMap(t, u.expect("POST", "/api/fields/"+levelID+"/archive", map[string]any{}, http.StatusConflict))
	if len(deps["dependencies"].([]any)) == 0 {
		t.Fatalf("archiving a formula input should identify its dependency: %#v", deps)
	}
	u.expect("POST", "/api/fields/"+notesID+"/archive", map[string]any{}, http.StatusNoContent)
	u.expect("POST", "/api/fields/"+notesID+"/restore", map[string]any{}, http.StatusNoContent)
	object = decodeMap(t, u.expect("GET", "/api/objects/"+objectID, nil, http.StatusOK))
	values = object["values"].(map[string]any)
	if values[notesID] != "kept" {
		t.Fatalf("archiving and restoring must retain field value, got %#v", values)
	}
	formulaOptions := map[string]any{"expression": "@{" + formulaID + "} + 1"}
	u.expect("PATCH", "/api/fields/"+formulaID, map[string]any{"options": formulaOptions}, http.StatusBadRequest)
	u.expect("PATCH", "/api/objects/"+objectID+"/values", map[string]any{"values": map[string]any{linkID: secondObjectID}}, http.StatusOK)
	// A reference is relationally validated and cannot cross game boundaries.
	otherGame := decodeMap(t, u.expect("POST", "/api/games", map[string]any{"name": "Other Game"}, http.StatusCreated))
	otherGameID := mapString(otherGame, "id")
	otherType := decodeMap(t, u.expect("POST", "/api/games/"+otherGameID+"/object-types", map[string]any{"name": "Anything"}, http.StatusCreated))
	otherTypeID := mapString(otherType, "id")
	cross := decodeMap(t, u.expect("POST", "/api/games/"+otherGameID+"/objects", map[string]any{"object_type_id": otherTypeID, "name": "Elsewhere"}, http.StatusCreated))
	crossID := mapString(cross, "id")
	u.expect("PATCH", "/api/objects/"+objectID+"/values", map[string]any{"values": map[string]any{linkID: crossID}}, http.StatusBadRequest)
	// Nested sections reject a move that would create an ancestor cycle.
	u.expect("PATCH", "/api/sections/"+rootID, map[string]any{"parent_id": childID}, http.StatusBadRequest)
}

func TestSectionDisplayConfigurationUsesOwnedCompatibleFields(t *testing.T) {
	e := newTestEnvironment(t)
	u := e.client()
	u.register("view_user")
	game := decodeMap(t, u.expect("POST", "/api/games", map[string]any{"name": "View Game"}, http.StatusCreated))
	gameID := mapString(game, "id")
	typeDef := decodeMap(t, u.expect("POST", "/api/games/"+gameID+"/object-types", map[string]any{"name": "Location"}, http.StatusCreated))
	typeID := mapString(typeDef, "id")
	makeField := func(name, kind string) string {
		field := decodeMap(t, u.expect("POST", "/api/object-types/"+typeID+"/fields", map[string]any{"name": name, "field_type": kind}, http.StatusCreated))
		return mapString(field, "id")
	}
	doneID := makeField("Visited", "boolean")
	progressID := makeField("Exploration", "progress")
	countID := makeField("Chests", "integer")
	config := map[string]any{"visible_field_ids": []string{countID}, "checklist_field_id": doneID, "progress_field_id": progressID}
	section := decodeMap(t, u.expect("POST", "/api/games/"+gameID+"/sections", map[string]any{"name": "Exploration", "display_style": "progress", "display_config": config}, http.StatusCreated))
	if section["display_style"] != "progress" {
		t.Fatalf("progress display style was not preserved: %#v", section)
	}
	sectionID := mapString(section, "id")
	dependencies := decodeMap(t, u.expect("POST", "/api/fields/"+progressID+"/archive", map[string]any{}, http.StatusConflict))
	if len(dependencies["dependencies"].([]any)) == 0 {
		t.Fatalf("progress field archive should identify its section view: %#v", dependencies)
	}
	u.expect("PATCH", "/api/sections/"+sectionID, map[string]any{"display_style": "checklist", "display_config": map[string]any{"checklist_field_id": countID}}, http.StatusBadRequest)
	otherGame := decodeMap(t, u.expect("POST", "/api/games", map[string]any{"name": "Other Game"}, http.StatusCreated))
	otherType := decodeMap(t, u.expect("POST", "/api/games/"+mapString(otherGame, "id")+"/object-types", map[string]any{"name": "Other"}, http.StatusCreated))
	otherField := decodeMap(t, u.expect("POST", "/api/object-types/"+mapString(otherType, "id")+"/fields", map[string]any{"name": "Foreign", "field_type": "boolean"}, http.StatusCreated))
	u.expect("PATCH", "/api/sections/"+sectionID, map[string]any{"display_config": map[string]any{"checklist_field_id": mapString(otherField, "id")}}, http.StatusBadRequest)
}

func TestUploadValidationAndOwnership(t *testing.T) {
	e := newTestEnvironment(t)
	u := e.client()
	u.register("asset_user")
	status, data := u.multipart("/api/assets", []byte("not an image"), "text.txt")
	if status != http.StatusUnsupportedMediaType {
		t.Fatalf("invalid upload status=%d body=%s", status, data)
	}
	var encodedPNG bytes.Buffer
	if err := png.Encode(&encodedPNG, image.NewRGBA(image.Rect(0, 0, 1, 1))); err != nil {
		t.Fatal(err)
	}
	status, data = u.multipart("/api/assets", encodedPNG.Bytes()[:33], "incomplete.png")
	if status != http.StatusUnsupportedMediaType {
		t.Fatalf("accepted incomplete pixel data: %d %s", status, data)
	}
	status, data = u.multipart("/api/assets", encodedPNG.Bytes(), "pixel.png")
	if status != http.StatusCreated {
		t.Fatalf("valid png upload: status=%d body=%s", status, data)
	}
	assetID := mapString(decodeMap(t, data), "id")
	u.expect("GET", "/api/assets/"+assetID, nil, http.StatusOK)
	game := decodeMap(t, u.expect("POST", "/api/games", map[string]any{"name": "Image owner", "cover_image": "/api/assets/" + assetID}, http.StatusCreated))
	u.expect("DELETE", "/api/assets/"+assetID, nil, http.StatusConflict)
	other := e.client()
	other.register("other_asset_user")
	other.expect("GET", "/api/assets/"+assetID, nil, http.StatusNotFound)
	other.expect("DELETE", "/api/assets/"+assetID, nil, http.StatusNotFound)
	u.expect("PATCH", "/api/games/"+mapString(game, "id"), map[string]any{"cover_image": ""}, http.StatusOK)
	u.expect("DELETE", "/api/assets/"+assetID, nil, http.StatusNoContent)
	u.expect("GET", "/api/assets/"+assetID, nil, http.StatusNotFound)

}

func TestStaleUnreferencedUploadsAreCleanedWithoutDeletingArchivedReferences(t *testing.T) {
	e := newTestEnvironment(t)
	u := e.client()
	u.register("stale_asset_user")
	var encodedPNG bytes.Buffer
	if err := png.Encode(&encodedPNG, image.NewRGBA(image.Rect(0, 0, 1, 1))); err != nil {
		t.Fatal(err)
	}
	upload := func() string {
		t.Helper()
		status, body := u.multipart("/api/assets", encodedPNG.Bytes(), "pixel.png")
		if status != http.StatusCreated {
			t.Fatalf("upload status=%d body=%s", status, body)
		}
		return mapString(decodeMap(t, body), "id")
	}
	usedID, orphanID := upload(), upload()
	game := decodeMap(t, u.expect("POST", "/api/games", map[string]any{"name": "Archived image", "cover_image": "/api/assets/" + usedID}, http.StatusCreated))
	gameID := mapString(game, "id")
	u.expect("POST", "/api/games/"+gameID+"/archive", map[string]any{}, http.StatusNoContent)
	if _, err := e.db.Exec(`UPDATE uploaded_assets SET created_at='2000-01-01T00:00:00Z' WHERE id IN (?,?)`, usedID, orphanID); err != nil {
		t.Fatal(err)
	}
	u.expect("GET", "/api/assets", nil, http.StatusOK)
	var usedCount, orphanCount int
	if err := e.db.QueryRow(`SELECT COUNT(*) FROM uploaded_assets WHERE id=?`, usedID).Scan(&usedCount); err != nil {
		t.Fatal(err)
	}
	if err := e.db.QueryRow(`SELECT COUNT(*) FROM uploaded_assets WHERE id=?`, orphanID).Scan(&orphanCount); err != nil {
		t.Fatal(err)
	}
	if usedCount != 1 || orphanCount != 0 {
		t.Fatalf("stale cleanup must preserve archived references and remove only unreferenced uploads: used=%d orphan=%d", usedCount, orphanCount)
	}
	u.expect("GET", "/api/assets/"+usedID, nil, http.StatusOK)
	u.expect("GET", "/api/assets/"+orphanID, nil, http.StatusNotFound)
}

func TestCustomFieldTypesAndNumericGoalProgress(t *testing.T) {
	e := newTestEnvironment(t)
	u := e.client()
	u.register("field_user")
	game := decodeMap(t, u.expect("POST", "/api/games", map[string]any{"name": "Data Types"}, http.StatusCreated))
	gameID := mapString(game, "id")
	typeDef := decodeMap(t, u.expect("POST", "/api/games/"+gameID+"/object-types", map[string]any{"name": "Record"}, http.StatusCreated))
	typeID := mapString(typeDef, "id")
	makeField := func(name, kind string, options any, defaultValue any) string {
		body := map[string]any{"name": name, "field_type": kind, "options": options}
		if defaultValue != nil {
			body["default_value"] = defaultValue
		}
		data := u.expect("POST", "/api/object-types/"+typeID+"/fields", body, http.StatusCreated)
		return mapString(decodeMap(t, data), "id")
	}
	textID := makeField("Notes", "long_text", map[string]any{}, nil)
	integerID := makeField("Count", "integer", map[string]any{}, 7)
	decimalID := makeField("Weight", "decimal", map[string]any{}, nil)
	percentID := makeField("Completion", "percentage", map[string]any{}, nil)
	booleanID := makeField("Done", "boolean", map[string]any{}, nil)
	selectID := makeField("State", "select", map[string]any{"choices": []string{"Active", "Waiting"}}, nil)
	dateID := makeField("Date", "date", map[string]any{}, nil)
	progressID := makeField("Progress", "progress", map[string]any{}, nil)
	referenceID := makeField("Related", "reference", map[string]any{"allowed_type_id": typeID}, nil)
	referencesID := makeField("Related items", "references", map[string]any{"allowed_type_id": typeID}, nil)
	formulaID := makeField("Score", "formula", map[string]any{"expression": "@{" + integerID + "} * @{" + decimalID + "}"}, nil)
	brokenFormulaID := makeField("Broken", "formula", map[string]any{"expression": "@{" + integerID + "} / 0"}, nil)
	target := decodeMap(t, u.expect("POST", "/api/games/"+gameID+"/objects", map[string]any{"object_type_id": typeID, "name": "Target"}, http.StatusCreated))
	targetID := mapString(target, "id")
	values := map[string]any{textID: "Long form notes", integerID: 3, decimalID: 2.5, percentID: 62.5, booleanID: true, selectID: "Active", dateID: "2026-11-04", progressID: map[string]any{"current": 4, "max": 10}, referenceID: targetID, referencesID: []string{targetID}}
	entry := decodeMap(t, u.expect("POST", "/api/games/"+gameID+"/objects", map[string]any{"object_type_id": typeID, "name": "Entry", "values": values}, http.StatusCreated))
	entryID := mapString(entry, "id")
	stored := decodeMap(t, u.expect("GET", "/api/objects/"+entryID, nil, http.StatusOK))
	storedValues := stored["values"].(map[string]any)
	if storedValues[integerID] != float64(3) || storedValues[decimalID] != 2.5 || storedValues[percentID] != 62.5 || storedValues[booleanID] != true || storedValues[selectID] != "Active" || storedValues[dateID] != "2026-11-04" || storedValues[formulaID] != 7.5 || storedValues[integerID] == nil {
		t.Fatalf("custom typed values were not preserved: %#v", storedValues)
	}
	if storedValues[textID] != "Long form notes" || storedValues[referenceID] != targetID || len(storedValues[referencesID].([]any)) != 1 || storedValues[integerID] != float64(3) {
		t.Fatalf("text or relational reference values were not preserved: %#v", storedValues)
	}
	if storedValues[brokenFormulaID] != nil {
		t.Fatalf("division-by-zero formula should not return a value: %#v", storedValues[brokenFormulaID])
	}
	formulaErrors := stored["formula_errors"].(map[string]any)
	if !strings.Contains(formulaErrors[brokenFormulaID].(string), "division by zero") {
		t.Fatalf("expected a useful formula error, got %#v", formulaErrors)
	}
	u.expect("PATCH", "/api/objects/"+entryID+"/values", map[string]any{"values": map[string]any{percentID: 101}}, http.StatusBadRequest)
	u.expect("PATCH", "/api/objects/"+entryID+"/values", map[string]any{"values": map[string]any{selectID: "Unavailable"}}, http.StatusBadRequest)
	u.expect("PATCH", "/api/objects/"+entryID+"/values", map[string]any{"values": map[string]any{dateID: "04-11-2026"}}, http.StatusBadRequest)
	u.expect("PATCH", "/api/objects/"+entryID+"/values", map[string]any{"values": map[string]any{progressID: map[string]any{"current": 2, "max": 0}}}, http.StatusBadRequest)
	goal := decodeMap(t, u.expect("POST", "/api/games/"+gameID+"/goals", map[string]any{"title": "Reach eight", "goal_type": "numeric", "current": 4, "target": 8}, http.StatusCreated))
	if goal["progress"] != float64(50) || goal["completed"] != false {
		t.Fatalf("numeric progress did not calculate correctly: %#v", goal)
	}
	goalID := mapString(goal, "id")
	u.expect("PATCH", "/api/goals/"+goalID, map[string]any{"current": 8}, http.StatusOK)
	goal = decodeMap(t, u.expect("GET", "/api/games/"+gameID+"/goals", nil, http.StatusOK))["items"].([]any)[0].(map[string]any)
	if goal["completed"] != true || goal["progress"] != float64(100) {
		t.Fatalf("numeric goal should complete at target: %#v", goal)
	}
}

func (u *testUser) multipart(path string, data []byte, filename string) (int, []byte) {
	var body bytes.Buffer
	writer := multipart.NewWriter(&body)
	file, err := writer.CreateFormFile("file", filename)
	if err != nil {
		u.t.Fatal(err)
	}
	if _, err = file.Write(data); err != nil {
		u.t.Fatal(err)
	}
	if err := writer.Close(); err != nil {
		u.t.Fatal(err)
	}
	req, err := http.NewRequest("POST", path, &body)
	if err != nil {
		u.t.Fatal(err)
	}
	req.Header.Set("Content-Type", writer.FormDataContentType())
	if u.csrf != "" {
		req.Header.Set("X-CSRF-Token", u.csrf)
	}
	if u.cookie != nil {
		req.AddCookie(u.cookie)
	}
	response := httptest.NewRecorder()
	u.handler.ServeHTTP(response, req)
	resp := response.Result()
	result, err := io.ReadAll(resp.Body)
	if err != nil {
		u.t.Fatal(err)
	}
	for _, cookie := range resp.Cookies() {
		if cookie.Name == "gametracker_session" && cookie.Value != "" {
			u.cookie = cookie
		}
	}
	return resp.StatusCode, result
}
