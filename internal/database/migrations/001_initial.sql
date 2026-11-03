CREATE TABLE users (
    id TEXT PRIMARY KEY,
    username TEXT NOT NULL COLLATE NOCASE UNIQUE,
    password_hash TEXT NOT NULL,
    created_at TEXT NOT NULL
);

CREATE TABLE sessions (
    token_hash TEXT PRIMARY KEY,
    csrf_token TEXT NOT NULL,
    user_id TEXT NOT NULL REFERENCES users(id) ON DELETE CASCADE,
    expires_at TEXT NOT NULL,
    created_at TEXT NOT NULL
);
CREATE INDEX sessions_user_idx ON sessions(user_id);
CREATE INDEX sessions_expiry_idx ON sessions(expires_at);

CREATE TABLE games (
    id TEXT PRIMARY KEY,
    user_id TEXT NOT NULL REFERENCES users(id) ON DELETE CASCADE,
    name TEXT NOT NULL,
    description TEXT NOT NULL DEFAULT '',
    cover_image TEXT NOT NULL DEFAULT '',
    personal_notes TEXT NOT NULL DEFAULT '',
    archived INTEGER NOT NULL DEFAULT 0 CHECK (archived IN (0,1)),
    created_at TEXT NOT NULL,
    updated_at TEXT NOT NULL,
    UNIQUE(id, user_id)
);
CREATE INDEX games_owner_sort_idx ON games(user_id, archived, updated_at DESC);

CREATE TABLE sections (
    id TEXT PRIMARY KEY,
    game_id TEXT NOT NULL REFERENCES games(id) ON DELETE CASCADE,
    parent_id TEXT,
    name TEXT NOT NULL,
    icon TEXT NOT NULL DEFAULT '✦',
    sort_order INTEGER NOT NULL DEFAULT 0,
    display_style TEXT NOT NULL DEFAULT 'cards',
    display_config TEXT NOT NULL DEFAULT '{}',
    archived INTEGER NOT NULL DEFAULT 0 CHECK (archived IN (0,1)),
    created_at TEXT NOT NULL,
    updated_at TEXT NOT NULL,
    UNIQUE(id, game_id),
    FOREIGN KEY(parent_id, game_id) REFERENCES sections(id, game_id) ON DELETE CASCADE
);
CREATE INDEX sections_game_order_idx ON sections(game_id, archived, parent_id, sort_order);

CREATE TABLE object_types (
    id TEXT PRIMARY KEY,
    game_id TEXT NOT NULL REFERENCES games(id) ON DELETE CASCADE,
    name TEXT NOT NULL,
    description TEXT NOT NULL DEFAULT '',
    icon TEXT NOT NULL DEFAULT '◈',
    image TEXT NOT NULL DEFAULT '',
    sort_order INTEGER NOT NULL DEFAULT 0,
    created_at TEXT NOT NULL,
    updated_at TEXT NOT NULL,
    UNIQUE(id, game_id)
);
CREATE INDEX object_types_game_idx ON object_types(game_id, sort_order);

CREATE TABLE field_definitions (
    id TEXT PRIMARY KEY,
    object_type_id TEXT NOT NULL REFERENCES object_types(id) ON DELETE CASCADE,
    name TEXT NOT NULL,
    field_type TEXT NOT NULL CHECK (field_type IN ('text','long_text','integer','decimal','percentage','boolean','select','date','progress','reference','references','formula')),
    icon TEXT NOT NULL DEFAULT '✦',
    description TEXT NOT NULL DEFAULT '',
    default_json TEXT,
    options_json TEXT NOT NULL DEFAULT '{}',
    sort_order INTEGER NOT NULL DEFAULT 0,
    archived INTEGER NOT NULL DEFAULT 0 CHECK (archived IN (0,1)),
    created_at TEXT NOT NULL,
    updated_at TEXT NOT NULL,
    UNIQUE(id, object_type_id)
);
CREATE INDEX fields_type_idx ON field_definitions(object_type_id, archived, sort_order);

CREATE TABLE objects (
    id TEXT PRIMARY KEY,
    game_id TEXT NOT NULL REFERENCES games(id) ON DELETE CASCADE,
    object_type_id TEXT NOT NULL,
    name TEXT NOT NULL,
    description TEXT NOT NULL DEFAULT '',
    image TEXT NOT NULL DEFAULT '',
    archived INTEGER NOT NULL DEFAULT 0 CHECK (archived IN (0,1)),
    created_at TEXT NOT NULL,
    updated_at TEXT NOT NULL,
    UNIQUE(id, game_id),
    UNIQUE(id, object_type_id),
    FOREIGN KEY(object_type_id, game_id) REFERENCES object_types(id, game_id) ON DELETE CASCADE
);
CREATE INDEX objects_game_type_idx ON objects(game_id, object_type_id, archived, updated_at DESC);

CREATE TABLE field_values (
    object_id TEXT NOT NULL,
    object_type_id TEXT NOT NULL,
    field_id TEXT NOT NULL,
    value_json TEXT NOT NULL,
    updated_at TEXT NOT NULL,
    PRIMARY KEY(object_id, field_id),
    FOREIGN KEY(object_id, object_type_id) REFERENCES objects(id, object_type_id) ON DELETE CASCADE,
    FOREIGN KEY(field_id, object_type_id) REFERENCES field_definitions(id, object_type_id) ON DELETE CASCADE
);
CREATE INDEX values_field_idx ON field_values(field_id, object_id);

CREATE TABLE object_relationships (
    game_id TEXT NOT NULL,
    object_id TEXT NOT NULL,
    object_type_id TEXT NOT NULL,
    field_id TEXT NOT NULL,
    referenced_object_id TEXT NOT NULL,
    PRIMARY KEY(object_id, field_id, referenced_object_id),
    FOREIGN KEY(object_id, object_type_id) REFERENCES objects(id, object_type_id) ON DELETE CASCADE,
    FOREIGN KEY(field_id, object_type_id) REFERENCES field_definitions(id, object_type_id) ON DELETE CASCADE,
    FOREIGN KEY(referenced_object_id, game_id) REFERENCES objects(id, game_id) ON DELETE CASCADE
);
CREATE INDEX relationships_target_idx ON object_relationships(referenced_object_id);

CREATE TABLE section_placements (
    section_id TEXT NOT NULL,
    game_id TEXT NOT NULL,
    object_id TEXT NOT NULL,
    created_at TEXT NOT NULL,
    PRIMARY KEY(section_id, object_id),
    FOREIGN KEY(section_id, game_id) REFERENCES sections(id, game_id) ON DELETE CASCADE,
    FOREIGN KEY(object_id, game_id) REFERENCES objects(id, game_id) ON DELETE CASCADE
);
CREATE INDEX placements_object_idx ON section_placements(object_id);

CREATE TABLE goals (
    id TEXT PRIMARY KEY,
    game_id TEXT NOT NULL REFERENCES games(id) ON DELETE CASCADE,
    title TEXT NOT NULL,
    description TEXT NOT NULL DEFAULT '',
    goal_type TEXT NOT NULL CHECK (goal_type IN ('manual','checklist','field_target','numeric')),
    object_id TEXT,
    field_id TEXT,
    target_json TEXT,
    current_value REAL NOT NULL DEFAULT 0,
    completed INTEGER NOT NULL DEFAULT 0 CHECK (completed IN (0,1)),
    sort_order INTEGER NOT NULL DEFAULT 0,
    created_at TEXT NOT NULL,
    updated_at TEXT NOT NULL,
    FOREIGN KEY(object_id, game_id) REFERENCES objects(id, game_id) ON DELETE RESTRICT,
    FOREIGN KEY(field_id) REFERENCES field_definitions(id) ON DELETE SET NULL
);
CREATE INDEX goals_game_idx ON goals(game_id, sort_order, created_at);

CREATE TABLE goal_tasks (
    id TEXT PRIMARY KEY,
    goal_id TEXT NOT NULL REFERENCES goals(id) ON DELETE CASCADE,
    title TEXT NOT NULL,
    completed INTEGER NOT NULL DEFAULT 0 CHECK (completed IN (0,1)),
    sort_order INTEGER NOT NULL DEFAULT 0,
    created_at TEXT NOT NULL
);
CREATE INDEX goal_tasks_goal_idx ON goal_tasks(goal_id, sort_order);

CREATE TABLE uploaded_assets (
    id TEXT PRIMARY KEY,
    user_id TEXT NOT NULL REFERENCES users(id) ON DELETE CASCADE,
    game_id TEXT REFERENCES games(id) ON DELETE CASCADE,
    filename TEXT NOT NULL,
    mime_type TEXT NOT NULL,
    size_bytes INTEGER NOT NULL,
    created_at TEXT NOT NULL
);
CREATE INDEX assets_owner_idx ON uploaded_assets(user_id, game_id);

CREATE TRIGGER field_type_immutable
BEFORE UPDATE OF field_type ON field_definitions
WHEN OLD.field_type <> NEW.field_type
BEGIN
    SELECT RAISE(ABORT, 'field_type is immutable');
END;
