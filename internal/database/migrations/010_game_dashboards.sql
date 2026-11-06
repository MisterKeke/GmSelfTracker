CREATE TABLE game_dashboards (
    game_id TEXT PRIMARY KEY REFERENCES games(id) ON DELETE CASCADE,
    layout_json TEXT NOT NULL CHECK (length(layout_json) <= 65536),
    updated_at TEXT NOT NULL
);

CREATE TRIGGER game_dashboard_asset_refs_delete AFTER DELETE ON game_dashboards
BEGIN
    DELETE FROM asset_references WHERE entity_type='dashboard' AND entity_id=OLD.game_id;
END;

CREATE TRIGGER game_dashboard_asset_refs_validate BEFORE INSERT ON asset_references
WHEN NEW.entity_type='dashboard' AND NOT EXISTS (
    SELECT 1 FROM game_dashboards d
    JOIN games g ON g.id=d.game_id
    JOIN uploaded_assets a ON a.id=NEW.asset_id
    WHERE d.game_id=NEW.entity_id AND a.user_id=g.user_id AND a.deleted_at IS NULL
)
BEGIN
    SELECT RAISE(ABORT,'dashboard image is unavailable or belongs to another account');
END;
