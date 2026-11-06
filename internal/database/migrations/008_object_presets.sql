CREATE TABLE object_presets (
    id TEXT PRIMARY KEY,
    object_id TEXT NOT NULL,
    game_id TEXT NOT NULL,
    name TEXT NOT NULL,
    description TEXT NOT NULL DEFAULT '',
    sort_order INTEGER NOT NULL DEFAULT 0,
    values_json TEXT NOT NULL DEFAULT '{}' CHECK (length(values_json) <= 1048576),
    created_at TEXT NOT NULL,
    updated_at TEXT NOT NULL,
    FOREIGN KEY(object_id, game_id) REFERENCES objects(id, game_id) ON DELETE CASCADE
);
CREATE INDEX object_presets_object_idx ON object_presets(object_id, sort_order, created_at, id);
