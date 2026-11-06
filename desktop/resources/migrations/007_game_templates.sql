CREATE TABLE game_templates (
    id TEXT PRIMARY KEY,
    user_id TEXT NOT NULL REFERENCES users(id) ON DELETE CASCADE,
    name TEXT NOT NULL,
    description TEXT NOT NULL DEFAULT '',
    icon TEXT NOT NULL DEFAULT '📘',
    template_version INTEGER NOT NULL DEFAULT 1 CHECK (template_version > 0),
    data_json TEXT NOT NULL CHECK (length(data_json) <= 1048576),
    created_at TEXT NOT NULL,
    updated_at TEXT NOT NULL
);
CREATE INDEX game_templates_owner_idx ON game_templates(user_id, updated_at DESC, id);
