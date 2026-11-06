CREATE TABLE object_history_events (
    id TEXT PRIMARY KEY,
    game_id TEXT NOT NULL REFERENCES games(id) ON DELETE CASCADE,
    object_id TEXT NOT NULL,
    object_name TEXT NOT NULL,
    user_id TEXT NOT NULL REFERENCES users(id) ON DELETE CASCADE,
    event_type TEXT NOT NULL CHECK (event_type IN ('created','updated','archived','restored','field_type_migrated')),
    source TEXT NOT NULL CHECK (source IN ('web','api','internal_migration')),
    created_at TEXT NOT NULL
);
CREATE INDEX object_history_object_idx ON object_history_events(game_id, object_id, created_at DESC, id DESC);

CREATE TABLE object_history_changes (
    id INTEGER PRIMARY KEY,
    event_id TEXT NOT NULL REFERENCES object_history_events(id) ON DELETE CASCADE,
    field_id TEXT NOT NULL DEFAULT '',
    field_name TEXT NOT NULL,
    field_type TEXT NOT NULL DEFAULT '',
    before_field_type TEXT NOT NULL DEFAULT '',
    before_json TEXT,
    after_json TEXT
);
CREATE INDEX object_history_change_field_idx ON object_history_changes(field_id, event_id);
