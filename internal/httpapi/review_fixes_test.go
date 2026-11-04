package httpapi

import (
	"context"
	"database/sql"
	"io"
	"log/slog"
	"net/http"
	"os"
	"path/filepath"
	"reflect"
	"testing"
	"time"
)

type reviewFixture struct {
	env            *testEnvironment
	user           *testUser
	gameID, typeID string
}

func newReviewFixture(t *testing.T) reviewFixture {
	e := newTestEnvironment(t)
	u := e.client()
	u.register("review_fixes")
	game := decodeMap(t, u.expect("POST", "/api/games", map[string]any{"name": "Review"}, http.StatusCreated))
	g := mapString(game, "id")
	typ := decodeMap(t, u.expect("POST", "/api/games/"+g+"/object-types", map[string]any{"name": "Entry"}, http.StatusCreated))
	return reviewFixture{e, u, g, mapString(typ, "id")}
}

func (f reviewFixture) field(t *testing.T, name, kind string, options map[string]any, defaultValue any) string {
	body := map[string]any{"name": name, "field_type": kind, "options": options}
	if defaultValue != nil {
		body["default_value"] = defaultValue
	}
	return mapString(decodeMap(t, f.user.expect("POST", "/api/object-types/"+f.typeID+"/fields", body, http.StatusCreated)), "id")
}

func (f reviewFixture) object(t *testing.T, name string, values map[string]any) string {
	return mapString(decodeMap(t, f.user.expect("POST", "/api/games/"+f.gameID+"/objects", map[string]any{"name": name, "object_type_id": f.typeID, "values": values}, http.StatusCreated)), "id")
}

func TestObjectEditRollsBackMetadataAndAllValues(t *testing.T) {
	f := newReviewFixture(t)
	n := f.field(t, "Count", "integer", map[string]any{}, nil)
	text := f.field(t, "Text", "text", map[string]any{}, nil)
	id := f.object(t, "Original", map[string]any{n: 1, text: "null"})
	badValues := map[string]any{text: "changed", n: "invalid"}
	if n < text {
		badValues = map[string]any{n: 2, text: 123}
	}
	f.user.expect("PATCH", "/api/objects/"+id, map[string]any{"name": "Changed", "description": "Should roll back", "values": badValues}, http.StatusBadRequest)
	object := decodeMap(t, f.user.expect("GET", "/api/objects/"+id, nil, http.StatusOK))
	if object["name"] != "Original" || object["description"] != "" || object["values"].(map[string]any)[n] != float64(1) || object["values"].(map[string]any)[text] != "null" {
		t.Fatalf("partial update persisted: %#v", object)
	}
	object = decodeMap(t, f.user.expect("PATCH", "/api/objects/"+id, map[string]any{"name": "Saved", "values": map[string]any{n: 2}}, http.StatusOK))
	if object["name"] != "Saved" || object["values"].(map[string]any)[n] != float64(2) {
		t.Fatalf("combined save failed: %#v", object)
	}
}

func TestExistingArchivedRelationshipsSurviveEdits(t *testing.T) {
	f := newReviewFixture(t)
	links := f.field(t, "Links", "references", map[string]any{}, nil)
	child := f.object(t, "Child", nil)
	parent := f.object(t, "Parent", map[string]any{links: []string{child}})
	f.user.expect("POST", "/api/objects/"+child+"/archive", map[string]any{}, http.StatusNoContent)
	f.user.expect("PATCH", "/api/objects/"+parent, map[string]any{"name": "Renamed", "values": map[string]any{links: []string{child}}}, http.StatusOK)
	f.user.expect("POST", "/api/games/"+f.gameID+"/objects", map[string]any{"name": "New parent", "object_type_id": f.typeID, "values": map[string]any{links: []string{child}}}, http.StatusBadRequest)
	f.user.expect("POST", "/api/objects/"+child+"/restore", map[string]any{}, http.StatusNoContent)
	object := decodeMap(t, f.user.expect("GET", "/api/objects/"+parent, nil, http.StatusOK))
	if object["values"].(map[string]any)[links].([]any)[0] != child {
		t.Fatal("restoring target lost the relationship")
	}
}

func TestBulkObjectPagePreservesIndividualObjectData(t *testing.T) {
	f := newReviewFixture(t)
	n := f.field(t, "Count", "integer", map[string]any{}, 3)
	links := f.field(t, "Links", "references", map[string]any{}, nil)
	f.field(t, "Double", "formula", map[string]any{"expression": "@{" + n + "} * 2"}, nil)
	child := f.object(t, "Child", nil)
	parent := f.object(t, "Parent", map[string]any{n: 7, links: []string{child}})
	section := mapString(decodeMap(t, f.user.expect("POST", "/api/games/"+f.gameID+"/sections", map[string]any{"name": "Placed"}, http.StatusCreated)), "id")
	f.user.expect("POST", "/api/sections/"+section+"/placements", map[string]any{"object_id": parent}, http.StatusNoContent)
	items := decodeMap(t, f.user.expect("GET", "/api/games/"+f.gameID+"/objects", nil, http.StatusOK))["items"].([]any)
	if len(items) != 2 {
		t.Fatalf("got %d objects", len(items))
	}
	for _, item := range items {
		object := item.(map[string]any)
		individual := decodeMap(t, f.user.expect("GET", "/api/objects/"+mapString(object, "id"), nil, http.StatusOK))
		if !reflect.DeepEqual(object, individual) {
			t.Fatalf("bulk hydration changed object data: %#v vs %#v", object, individual)
		}
	}
}

func TestSessionCreationRejectsCredentialsChangedAfterVerification(t *testing.T) {
	f := newReviewFixture(t)
	var id, oldHash string
	if err := f.env.db.QueryRow(`SELECT id,password_hash FROM users WHERE username='review_fixes'`).Scan(&id, &oldHash); err != nil {
		t.Fatal(err)
	}
	f.user.expect("POST", "/api/auth/password", map[string]any{"current_password": "safe-password-123", "password": "replacement-password", "confirm_password": "replacement-password"}, http.StatusNoContent)
	a := &API{db: f.env.db}
	_, _, err := a.newSession(context.Background(), id, oldHash)
	if err != errCredentialsChanged {
		t.Fatalf("stale verified hash created a session: %v", err)
	}
	var sessions int
	if err := f.env.db.QueryRow(`SELECT COUNT(*) FROM sessions WHERE user_id=?`, id).Scan(&sessions); err != nil {
		t.Fatal(err)
	}
	if sessions != 1 {
		t.Fatalf("unexpected session count %d", sessions)
	}
}

func TestSelectChangesRequireMigrationForObjectsDefaultsAndRules(t *testing.T) {
	f := newReviewFixture(t)
	choice := f.field(t, "State", "select", map[string]any{"choices": []string{"old", "new"}}, "old")
	id := f.object(t, "Entry", map[string]any{choice: "old"})
	f.user.expect("POST", "/api/games/"+f.gameID+"/goals", map[string]any{"title": "State goal", "goal_type": "field_target", "mode": "all", "conditions": []map[string]any{{"object_id": id, "field_id": choice, "operator": "eq", "value": "old"}}}, http.StatusCreated)
	f.user.expect("POST", "/api/objects/"+id+"/archive", map[string]any{}, http.StatusNoContent)
	options := map[string]any{"choices": []string{"new"}}
	f.user.expect("PATCH", "/api/fields/"+choice, map[string]any{"options": options}, http.StatusConflict)
	field := decodeMap(t, f.user.expect("PATCH", "/api/fields/"+choice, map[string]any{"options": options, "choice_replacements": map[string]string{"old": "new"}}, http.StatusOK))
	if field["default_value"] != "new" {
		t.Fatalf("default not migrated: %#v", field)
	}
	f.user.expect("POST", "/api/objects/"+id+"/restore", map[string]any{}, http.StatusNoContent)
	object := decodeMap(t, f.user.expect("GET", "/api/objects/"+id, nil, http.StatusOK))
	if object["values"].(map[string]any)[choice] != "new" {
		t.Fatal("archived object's choice was not migrated")
	}
	goals := decodeMap(t, f.user.expect("GET", "/api/games/"+f.gameID+"/goals", nil, http.StatusOK))["items"].([]any)
	rule := goals[0].(map[string]any)["target"].(map[string]any)
	if rule["conditions"].([]any)[0].(map[string]any)["value"] != "new" {
		t.Fatal("rule target not migrated")
	}
	f.user.expect("PATCH", "/api/fields/"+choice, map[string]any{"default_value": nil}, http.StatusOK)
	fieldType := decodeMap(t, f.user.expect("GET", "/api/object-types/"+f.typeID, nil, http.StatusOK))
	if _, ok := fieldType["fields"].([]any)[0].(map[string]any)["default_value"]; ok {
		t.Fatal("cleared default still present")
	}
}

func TestReferenceTypeChangesRequireExplicitRemoval(t *testing.T) {
	f := newReviewFixture(t)
	links := f.field(t, "Links", "references", map[string]any{}, nil)
	child := f.object(t, "Child", nil)
	parent := f.object(t, "Parent", map[string]any{links: []string{child}})
	other := decodeMap(t, f.user.expect("POST", "/api/games/"+f.gameID+"/object-types", map[string]any{"name": "Other"}, http.StatusCreated))
	options := map[string]any{"allowed_type_id": mapString(other, "id")}
	f.user.expect("PATCH", "/api/fields/"+links, map[string]any{"options": options}, http.StatusConflict)
	f.user.expect("PATCH", "/api/fields/"+links, map[string]any{"options": options, "remove_incompatible_references": true}, http.StatusOK)
	var count int
	if err := f.env.db.QueryRow(`SELECT COUNT(*) FROM object_relationships WHERE object_id=?`, parent).Scan(&count); err != nil {
		t.Fatal(err)
	}
	if count != 0 {
		t.Fatal("incompatible references were not removed")
	}
}

func TestImageDeletionQueuesBeforeRetryableFilesystemCleanup(t *testing.T) {
	e := newTestEnvironment(t)
	dir := t.TempDir()
	u := &testUser{t: t, handler: New(e.db, dir, false, slog.New(slog.NewTextHandler(io.Discard, nil)))}
	u.register("cleanup_user")
	var owner string
	if err := e.db.QueryRow(`SELECT id FROM users WHERE username='cleanup_user'`).Scan(&owner); err != nil {
		t.Fatal(err)
	}
	id, err := randomID()
	if err != nil {
		t.Fatal(err)
	}
	filename := id + ".png"
	blocked := filepath.Join(dir, filename)
	if err := os.Mkdir(blocked, 0o700); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(blocked, "keep"), []byte("blocking cleanup"), 0o600); err != nil {
		t.Fatal(err)
	}
	if _, err := e.db.Exec(`INSERT INTO uploaded_assets(id,user_id,filename,mime_type,size_bytes,created_at) VALUES(?,?,?,?,?,?)`, id, owner, filename, "image/png", 1, time.Now().UTC().Format(time.RFC3339)); err != nil {
		t.Fatal(err)
	}
	game := decodeMap(t, u.expect("POST", "/api/games", map[string]any{"name": "Image", "cover_image": "http://localhost/api/assets/" + id}, http.StatusCreated))
	u.expect("DELETE", "/api/assets/"+id, nil, http.StatusConflict)
	u.expect("PATCH", "/api/games/"+mapString(game, "id"), map[string]any{"cover_image": ""}, http.StatusOK)
	u.expect("DELETE", "/api/assets/"+id, nil, http.StatusAccepted)
	var deleted sql.NullString
	if err := e.db.QueryRow(`SELECT deleted_at FROM uploaded_assets WHERE id=?`, id).Scan(&deleted); err != nil || !deleted.Valid {
		t.Fatalf("missing durable deletion intent: %v", err)
	}
	u.expect("GET", "/api/assets/"+id, nil, http.StatusNotFound)
	u.expect("POST", "/api/games", map[string]any{"name": "Cannot reuse", "cover_image": "/api/assets/" + id}, http.StatusBadRequest)
	if err := os.Remove(filepath.Join(blocked, "keep")); err != nil {
		t.Fatal(err)
	}
	u.expect("DELETE", "/api/assets/"+id, nil, http.StatusNoContent)
	var count int
	if err := e.db.QueryRow(`SELECT COUNT(*) FROM uploaded_assets WHERE id=?`, id).Scan(&count); err != nil || count != 0 {
		t.Fatalf("cleanup did not finish: %v count=%d", err, count)
	}
}
