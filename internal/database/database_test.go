package database

import (
	"context"
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
	if version != 2 {
		t.Fatalf("migration version = %d, want 2", version)
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
