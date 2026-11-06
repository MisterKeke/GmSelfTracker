CREATE TABLE field_type_migrations (
    id TEXT PRIMARY KEY,
    game_id TEXT NOT NULL REFERENCES games(id) ON DELETE CASCADE,
    field_id TEXT NOT NULL,
    field_name TEXT NOT NULL,
    previous_type TEXT NOT NULL,
    new_type TEXT NOT NULL,
    policy_json TEXT NOT NULL DEFAULT '{}',
    previous_default_json TEXT,
    new_default_json TEXT,
    converted_values INTEGER NOT NULL DEFAULT 0 CHECK (converted_values >= 0),
    affected_objects INTEGER NOT NULL DEFAULT 0 CHECK (affected_objects >= 0),
    archived_objects_affected INTEGER NOT NULL DEFAULT 0 CHECK (archived_objects_affected >= 0),
    incompatible_values INTEGER NOT NULL DEFAULT 0 CHECK (incompatible_values >= 0),
    outcome TEXT NOT NULL CHECK (outcome IN ('applying','applied')),
    created_at TEXT NOT NULL
);
CREATE INDEX field_type_migrations_field_idx ON field_type_migrations(game_id, field_id, created_at DESC);

DROP TRIGGER field_type_immutable;
CREATE TRIGGER field_type_migration_guard
BEFORE UPDATE OF field_type ON field_definitions
WHEN OLD.field_type <> NEW.field_type
 AND NOT EXISTS (
    SELECT 1 FROM field_type_migrations m
    WHERE m.field_id=OLD.id
      AND m.previous_type=OLD.field_type
      AND m.new_type=NEW.field_type
      AND m.outcome='applying'
 )
BEGIN
    SELECT RAISE(ABORT, 'field_type change requires a validated migration');
END;
