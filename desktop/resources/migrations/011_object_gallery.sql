CREATE TABLE object_gallery (
    id TEXT PRIMARY KEY,
    object_id TEXT NOT NULL REFERENCES objects(id) ON DELETE CASCADE,
    asset_id TEXT NOT NULL REFERENCES uploaded_assets(id) ON DELETE RESTRICT,
    caption TEXT NOT NULL DEFAULT '',
    sort_order INTEGER NOT NULL DEFAULT 0 CHECK (sort_order >= 0),
    is_primary INTEGER NOT NULL DEFAULT 0 CHECK (is_primary IN (0,1)),
    created_at TEXT NOT NULL
);
CREATE INDEX object_gallery_object_order_idx ON object_gallery(object_id,sort_order,id);
CREATE UNIQUE INDEX object_gallery_primary_idx ON object_gallery(object_id) WHERE is_primary=1;

CREATE TRIGGER object_gallery_validate_insert BEFORE INSERT ON object_gallery
WHEN NOT EXISTS (
    SELECT 1 FROM uploaded_assets a
    JOIN objects o ON o.id=NEW.object_id
    JOIN games g ON g.id=o.game_id
    JOIN object_types t ON t.id=o.object_type_id
    WHERE a.id=NEW.asset_id AND a.user_id=g.user_id AND a.deleted_at IS NULL
      AND o.archived=0 AND t.archived=0 AND g.archived=0
)
BEGIN
    SELECT RAISE(ABORT,'gallery image or object is unavailable');
END;

CREATE TRIGGER object_gallery_validate_asset_update BEFORE UPDATE OF asset_id ON object_gallery
WHEN NOT EXISTS (
    SELECT 1 FROM uploaded_assets a
    JOIN objects o ON o.id=NEW.object_id
    JOIN games g ON g.id=o.game_id
    JOIN object_types t ON t.id=o.object_type_id
    WHERE a.id=NEW.asset_id AND a.user_id=g.user_id AND a.deleted_at IS NULL
      AND o.archived=0 AND t.archived=0 AND g.archived=0
)
BEGIN
    SELECT RAISE(ABORT,'gallery image or object is unavailable');
END;

CREATE TRIGGER object_gallery_asset_ref_insert AFTER INSERT ON object_gallery
BEGIN
    INSERT INTO asset_references(entity_type,entity_id,slot,asset_id)
    VALUES('object_gallery',NEW.id,'image',NEW.asset_id);
END;
CREATE TRIGGER object_gallery_asset_ref_update AFTER UPDATE OF asset_id ON object_gallery
BEGIN
    DELETE FROM asset_references WHERE entity_type='object_gallery' AND entity_id=OLD.id;
    INSERT INTO asset_references(entity_type,entity_id,slot,asset_id)
    VALUES('object_gallery',NEW.id,'image',NEW.asset_id);
END;
CREATE TRIGGER object_gallery_asset_ref_delete AFTER DELETE ON object_gallery
BEGIN
    DELETE FROM asset_references WHERE entity_type='object_gallery' AND entity_id=OLD.id;
END;
