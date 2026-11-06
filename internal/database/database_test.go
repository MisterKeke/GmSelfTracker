package database

import (
	"context"
	"database/sql"
	"fmt"
	"path/filepath"
	"testing"
)

func TestOpenAppliesMigrationAndEnablesIntegrity(t *testing.T) {
	path := filepath.Join(t.TempDir(), "tracker.db")
	db, err := Open(context.Background(), path)
	if err != nil {
		t.Fatal(err)
	}
	defer db.Close()
	var version, foreignKeys int
	if err := db.QueryRow(`SELECT MAX(version) FROM schema_migrations`).Scan(&version); err != nil {
		t.Fatal(err)
	}
	if version != 11 {
		t.Fatalf("migration version = %d, want 11", version)
	}
	if err := db.QueryRow(`PRAGMA foreign_keys`).Scan(&foreignKeys); err != nil {
		t.Fatal(err)
	}
	if foreignKeys != 1 {
		t.Fatalf("foreign_keys = %d, want 1", foreignKeys)
	}
	var tables int
	if err := db.QueryRow(`SELECT COUNT(*) FROM sqlite_master WHERE type='table' AND name IN ('games','sections','object_types','field_definitions','objects','field_values','section_placements','goals')`).Scan(&tables); err != nil {
		t.Fatal(err)
	}
	if tables != 8 {
		t.Fatalf("core table count = %d, want 8", tables)
	}
}

func TestUpgradePreviousSchemaPreservesData(t *testing.T) {
	path := filepath.Join(t.TempDir(), "previous.db")
	db, err := sql.Open("sqlite", path)
	if err != nil {
		t.Fatal(err)
	}
	db.SetMaxOpenConns(1)
	if _, err = db.Exec(`PRAGMA foreign_keys=ON; CREATE TABLE schema_migrations(version INTEGER PRIMARY KEY,applied_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP)`); err != nil {
		t.Fatal(err)
	}
	for i, name := range []string{"001_initial.sql", "002_local_features.sql", "003_asset_lifecycle.sql"} {
		raw, err := migrationFS.ReadFile("migrations/" + name)
		if err != nil {
			t.Fatal(err)
		}
		if _, err = db.Exec(string(raw)); err != nil {
			t.Fatalf("previous migration %s: %v", name, err)
		}
		if _, err = db.Exec(`INSERT INTO schema_migrations(version) VALUES(?)`, i+1); err != nil {
			t.Fatal(err)
		}
	}
	const fixture = `
 INSERT INTO users(id,username,password_hash,created_at) VALUES('u','previous','hash','original');
 INSERT INTO games(id,user_id,name,created_at,updated_at) VALUES('g','u','Saved game','original','original');
 INSERT INTO object_types(id,game_id,name,created_at,updated_at) VALUES('t','g','Saved type','original','original');
 INSERT INTO field_definitions(id,object_type_id,name,field_type,archived,created_at,updated_at) VALUES('f','t','Saved field','decimal',1,'original','original');
 INSERT INTO objects(id,game_id,object_type_id,name,created_at,updated_at) VALUES('o','g','t','Saved object','original','original');
 INSERT INTO field_values(object_id,object_type_id,field_id,value_json,updated_at) VALUES('o','t','f','42','original');
 INSERT INTO sections(id,game_id,name,created_at,updated_at) VALUES('s','g','Saved section','original','original');
 INSERT INTO section_placements(section_id,game_id,object_id,created_at) VALUES('s','g','o','original');
 INSERT INTO goals(id,game_id,title,goal_type,created_at,updated_at) VALUES('goal','g','Saved goal','manual','original','original');
 INSERT INTO section_goal_placements(section_id,game_id,goal_id,created_at) VALUES('s','g','goal','original');`
	if _, err = db.Exec(fixture); err != nil {
		t.Fatal(err)
	}
	db.Close()
	db, err = Open(context.Background(), path)
	if err != nil {
		t.Fatal(err)
	}
	defer db.Close()
	var primary, raw string
	var archived, objects, goals int
	if err = db.QueryRow(`SELECT primary_display_field_id FROM object_types WHERE id='t'`).Scan(&primary); err != nil || primary != "" {
		t.Fatalf("legacy primary=%q: %v", primary, err)
	}
	if err = db.QueryRow(`SELECT v.value_json,f.archived FROM field_values v JOIN field_definitions f ON f.id=v.field_id WHERE v.object_id='o' AND v.field_id='f'`).Scan(&raw, &archived); err != nil || raw != "42" || archived != 1 {
		t.Fatalf("archived data: %q %d %v", raw, archived, err)
	}
	db.QueryRow(`SELECT COUNT(*) FROM section_placements WHERE section_id='s' AND object_id='o'`).Scan(&objects)
	db.QueryRow(`SELECT COUNT(*) FROM section_goal_placements WHERE section_id='s' AND goal_id='goal'`).Scan(&goals)
	if objects != 1 || goals != 1 {
		t.Fatal("migration changed placements")
	}
	if _, err = db.Exec(`UPDATE object_types SET primary_display_field_id='f' WHERE id='t'`); err != nil {
		t.Fatal(err)
	}
	if _, err = db.Exec(`UPDATE object_types SET primary_display_field_id='invalid' WHERE id='t'`); err == nil {
		t.Fatal("invalid primary field accepted by database")
	}
	if err = Migrate(context.Background(), db); err != nil {
		t.Fatal(fmt.Errorf("repeat migration: %w", err))
	}
	var violations int
	rows, err := db.Query(`PRAGMA foreign_key_check`)
	if err != nil {
		t.Fatal(err)
	}
	for rows.Next() {
		violations++
	}
	rows.Close()
	if violations != 0 {
		t.Fatal("foreign key violations after upgrade")
	}
}
