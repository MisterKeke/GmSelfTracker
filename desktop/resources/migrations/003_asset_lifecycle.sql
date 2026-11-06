-- Deleted images remain queued until their files have been removed successfully.
ALTER TABLE uploaded_assets ADD COLUMN deleted_at TEXT;
CREATE INDEX assets_cleanup_idx ON uploaded_assets(deleted_at);
CREATE TABLE asset_references (
 entity_type TEXT NOT NULL,
 entity_id TEXT NOT NULL,
 slot TEXT NOT NULL,
 asset_id TEXT NOT NULL REFERENCES uploaded_assets(id) ON DELETE RESTRICT,
 PRIMARY KEY(entity_type,entity_id,slot)
);
CREATE INDEX asset_references_asset_idx ON asset_references(asset_id);

INSERT INTO asset_references SELECT 'game',s.id,'icon',a.id FROM games s JOIN uploaded_assets a ON a.id=substr(s.icon,instr(s.icon,'/api/assets/')+12,36) AND a.user_id=s.user_id WHERE instr(s.icon,'/api/assets/')>0;

INSERT INTO asset_references SELECT 'game',s.id,'cover_image',a.id FROM games s JOIN uploaded_assets a ON a.id=substr(s.cover_image,instr(s.cover_image,'/api/assets/')+12,36) AND a.user_id=s.user_id WHERE instr(s.cover_image,'/api/assets/')>0;

CREATE TRIGGER games_asset_refs_insert AFTER INSERT ON games
BEGIN
 DELETE FROM asset_references WHERE entity_type='game' AND entity_id=NEW.id;
 SELECT CASE WHEN substr(NEW.icon,1,12)='/api/assets/' AND NOT EXISTS (SELECT 1 FROM uploaded_assets WHERE id=substr(NEW.icon,instr(NEW.icon,'/api/assets/')+12,36) AND user_id=NEW.user_id AND deleted_at IS NULL) THEN RAISE(ABORT,'image is unavailable or belongs to another account') END;
 INSERT INTO asset_references SELECT 'game',NEW.id,'icon',a.id FROM uploaded_assets a WHERE instr(NEW.icon,'/api/assets/')>0 AND a.id=substr(NEW.icon,instr(NEW.icon,'/api/assets/')+12,36) AND a.user_id=NEW.user_id AND a.deleted_at IS NULL;
 SELECT CASE WHEN substr(NEW.cover_image,1,12)='/api/assets/' AND NOT EXISTS (SELECT 1 FROM uploaded_assets WHERE id=substr(NEW.cover_image,instr(NEW.cover_image,'/api/assets/')+12,36) AND user_id=NEW.user_id AND deleted_at IS NULL) THEN RAISE(ABORT,'image is unavailable or belongs to another account') END;
 INSERT INTO asset_references SELECT 'game',NEW.id,'cover_image',a.id FROM uploaded_assets a WHERE instr(NEW.cover_image,'/api/assets/')>0 AND a.id=substr(NEW.cover_image,instr(NEW.cover_image,'/api/assets/')+12,36) AND a.user_id=NEW.user_id AND a.deleted_at IS NULL;
END;

CREATE TRIGGER games_asset_refs_update AFTER UPDATE ON games
BEGIN
 DELETE FROM asset_references WHERE entity_type='game' AND entity_id=NEW.id;
 SELECT CASE WHEN substr(NEW.icon,1,12)='/api/assets/' AND NOT EXISTS (SELECT 1 FROM uploaded_assets WHERE id=substr(NEW.icon,instr(NEW.icon,'/api/assets/')+12,36) AND user_id=NEW.user_id AND deleted_at IS NULL) THEN RAISE(ABORT,'image is unavailable or belongs to another account') END;
 INSERT INTO asset_references SELECT 'game',NEW.id,'icon',a.id FROM uploaded_assets a WHERE instr(NEW.icon,'/api/assets/')>0 AND a.id=substr(NEW.icon,instr(NEW.icon,'/api/assets/')+12,36) AND a.user_id=NEW.user_id AND a.deleted_at IS NULL;
 SELECT CASE WHEN substr(NEW.cover_image,1,12)='/api/assets/' AND NOT EXISTS (SELECT 1 FROM uploaded_assets WHERE id=substr(NEW.cover_image,instr(NEW.cover_image,'/api/assets/')+12,36) AND user_id=NEW.user_id AND deleted_at IS NULL) THEN RAISE(ABORT,'image is unavailable or belongs to another account') END;
 INSERT INTO asset_references SELECT 'game',NEW.id,'cover_image',a.id FROM uploaded_assets a WHERE instr(NEW.cover_image,'/api/assets/')>0 AND a.id=substr(NEW.cover_image,instr(NEW.cover_image,'/api/assets/')+12,36) AND a.user_id=NEW.user_id AND a.deleted_at IS NULL;
END;

CREATE TRIGGER games_asset_refs_delete AFTER DELETE ON games
BEGIN DELETE FROM asset_references WHERE entity_type='game' AND entity_id=OLD.id; END;

INSERT INTO asset_references SELECT 'section',s.id,'icon',a.id FROM sections s JOIN uploaded_assets a ON a.id=substr(s.icon,instr(s.icon,'/api/assets/')+12,36) AND a.user_id=(SELECT user_id FROM games WHERE id=s.game_id) WHERE instr(s.icon,'/api/assets/')>0;

CREATE TRIGGER sections_asset_refs_insert AFTER INSERT ON sections
BEGIN
 DELETE FROM asset_references WHERE entity_type='section' AND entity_id=NEW.id;
 SELECT CASE WHEN substr(NEW.icon,1,12)='/api/assets/' AND NOT EXISTS (SELECT 1 FROM uploaded_assets WHERE id=substr(NEW.icon,instr(NEW.icon,'/api/assets/')+12,36) AND user_id=(SELECT user_id FROM games WHERE id=NEW.game_id) AND deleted_at IS NULL) THEN RAISE(ABORT,'image is unavailable or belongs to another account') END;
 INSERT INTO asset_references SELECT 'section',NEW.id,'icon',a.id FROM uploaded_assets a WHERE instr(NEW.icon,'/api/assets/')>0 AND a.id=substr(NEW.icon,instr(NEW.icon,'/api/assets/')+12,36) AND a.user_id=(SELECT user_id FROM games WHERE id=NEW.game_id) AND a.deleted_at IS NULL;
END;

CREATE TRIGGER sections_asset_refs_update AFTER UPDATE ON sections
BEGIN
 DELETE FROM asset_references WHERE entity_type='section' AND entity_id=NEW.id;
 SELECT CASE WHEN substr(NEW.icon,1,12)='/api/assets/' AND NOT EXISTS (SELECT 1 FROM uploaded_assets WHERE id=substr(NEW.icon,instr(NEW.icon,'/api/assets/')+12,36) AND user_id=(SELECT user_id FROM games WHERE id=NEW.game_id) AND deleted_at IS NULL) THEN RAISE(ABORT,'image is unavailable or belongs to another account') END;
 INSERT INTO asset_references SELECT 'section',NEW.id,'icon',a.id FROM uploaded_assets a WHERE instr(NEW.icon,'/api/assets/')>0 AND a.id=substr(NEW.icon,instr(NEW.icon,'/api/assets/')+12,36) AND a.user_id=(SELECT user_id FROM games WHERE id=NEW.game_id) AND a.deleted_at IS NULL;
END;

CREATE TRIGGER sections_asset_refs_delete AFTER DELETE ON sections
BEGIN DELETE FROM asset_references WHERE entity_type='section' AND entity_id=OLD.id; END;

INSERT INTO asset_references SELECT 'type',s.id,'icon',a.id FROM object_types s JOIN uploaded_assets a ON a.id=substr(s.icon,instr(s.icon,'/api/assets/')+12,36) AND a.user_id=(SELECT user_id FROM games WHERE id=s.game_id) WHERE instr(s.icon,'/api/assets/')>0;

INSERT INTO asset_references SELECT 'type',s.id,'image',a.id FROM object_types s JOIN uploaded_assets a ON a.id=substr(s.image,instr(s.image,'/api/assets/')+12,36) AND a.user_id=(SELECT user_id FROM games WHERE id=s.game_id) WHERE instr(s.image,'/api/assets/')>0;

CREATE TRIGGER object_types_asset_refs_insert AFTER INSERT ON object_types
BEGIN
 DELETE FROM asset_references WHERE entity_type='type' AND entity_id=NEW.id;
 SELECT CASE WHEN substr(NEW.icon,1,12)='/api/assets/' AND NOT EXISTS (SELECT 1 FROM uploaded_assets WHERE id=substr(NEW.icon,instr(NEW.icon,'/api/assets/')+12,36) AND user_id=(SELECT user_id FROM games WHERE id=NEW.game_id) AND deleted_at IS NULL) THEN RAISE(ABORT,'image is unavailable or belongs to another account') END;
 INSERT INTO asset_references SELECT 'type',NEW.id,'icon',a.id FROM uploaded_assets a WHERE instr(NEW.icon,'/api/assets/')>0 AND a.id=substr(NEW.icon,instr(NEW.icon,'/api/assets/')+12,36) AND a.user_id=(SELECT user_id FROM games WHERE id=NEW.game_id) AND a.deleted_at IS NULL;
 SELECT CASE WHEN substr(NEW.image,1,12)='/api/assets/' AND NOT EXISTS (SELECT 1 FROM uploaded_assets WHERE id=substr(NEW.image,instr(NEW.image,'/api/assets/')+12,36) AND user_id=(SELECT user_id FROM games WHERE id=NEW.game_id) AND deleted_at IS NULL) THEN RAISE(ABORT,'image is unavailable or belongs to another account') END;
 INSERT INTO asset_references SELECT 'type',NEW.id,'image',a.id FROM uploaded_assets a WHERE instr(NEW.image,'/api/assets/')>0 AND a.id=substr(NEW.image,instr(NEW.image,'/api/assets/')+12,36) AND a.user_id=(SELECT user_id FROM games WHERE id=NEW.game_id) AND a.deleted_at IS NULL;
END;

CREATE TRIGGER object_types_asset_refs_update AFTER UPDATE ON object_types
BEGIN
 DELETE FROM asset_references WHERE entity_type='type' AND entity_id=NEW.id;
 SELECT CASE WHEN substr(NEW.icon,1,12)='/api/assets/' AND NOT EXISTS (SELECT 1 FROM uploaded_assets WHERE id=substr(NEW.icon,instr(NEW.icon,'/api/assets/')+12,36) AND user_id=(SELECT user_id FROM games WHERE id=NEW.game_id) AND deleted_at IS NULL) THEN RAISE(ABORT,'image is unavailable or belongs to another account') END;
 INSERT INTO asset_references SELECT 'type',NEW.id,'icon',a.id FROM uploaded_assets a WHERE instr(NEW.icon,'/api/assets/')>0 AND a.id=substr(NEW.icon,instr(NEW.icon,'/api/assets/')+12,36) AND a.user_id=(SELECT user_id FROM games WHERE id=NEW.game_id) AND a.deleted_at IS NULL;
 SELECT CASE WHEN substr(NEW.image,1,12)='/api/assets/' AND NOT EXISTS (SELECT 1 FROM uploaded_assets WHERE id=substr(NEW.image,instr(NEW.image,'/api/assets/')+12,36) AND user_id=(SELECT user_id FROM games WHERE id=NEW.game_id) AND deleted_at IS NULL) THEN RAISE(ABORT,'image is unavailable or belongs to another account') END;
 INSERT INTO asset_references SELECT 'type',NEW.id,'image',a.id FROM uploaded_assets a WHERE instr(NEW.image,'/api/assets/')>0 AND a.id=substr(NEW.image,instr(NEW.image,'/api/assets/')+12,36) AND a.user_id=(SELECT user_id FROM games WHERE id=NEW.game_id) AND a.deleted_at IS NULL;
END;

CREATE TRIGGER object_types_asset_refs_delete AFTER DELETE ON object_types
BEGIN DELETE FROM asset_references WHERE entity_type='type' AND entity_id=OLD.id; END;

INSERT INTO asset_references SELECT 'field',s.id,'icon',a.id FROM field_definitions s JOIN uploaded_assets a ON a.id=substr(s.icon,instr(s.icon,'/api/assets/')+12,36) AND a.user_id=(SELECT g.user_id FROM object_types t JOIN games g ON g.id=t.game_id WHERE t.id=s.object_type_id) WHERE instr(s.icon,'/api/assets/')>0;

CREATE TRIGGER field_definitions_asset_refs_insert AFTER INSERT ON field_definitions
BEGIN
 DELETE FROM asset_references WHERE entity_type='field' AND entity_id=NEW.id;
 SELECT CASE WHEN substr(NEW.icon,1,12)='/api/assets/' AND NOT EXISTS (SELECT 1 FROM uploaded_assets WHERE id=substr(NEW.icon,instr(NEW.icon,'/api/assets/')+12,36) AND user_id=(SELECT g.user_id FROM object_types t JOIN games g ON g.id=t.game_id WHERE t.id=NEW.object_type_id) AND deleted_at IS NULL) THEN RAISE(ABORT,'image is unavailable or belongs to another account') END;
 INSERT INTO asset_references SELECT 'field',NEW.id,'icon',a.id FROM uploaded_assets a WHERE instr(NEW.icon,'/api/assets/')>0 AND a.id=substr(NEW.icon,instr(NEW.icon,'/api/assets/')+12,36) AND a.user_id=(SELECT g.user_id FROM object_types t JOIN games g ON g.id=t.game_id WHERE t.id=NEW.object_type_id) AND a.deleted_at IS NULL;
END;

CREATE TRIGGER field_definitions_asset_refs_update AFTER UPDATE ON field_definitions
BEGIN
 DELETE FROM asset_references WHERE entity_type='field' AND entity_id=NEW.id;
 SELECT CASE WHEN substr(NEW.icon,1,12)='/api/assets/' AND NOT EXISTS (SELECT 1 FROM uploaded_assets WHERE id=substr(NEW.icon,instr(NEW.icon,'/api/assets/')+12,36) AND user_id=(SELECT g.user_id FROM object_types t JOIN games g ON g.id=t.game_id WHERE t.id=NEW.object_type_id) AND deleted_at IS NULL) THEN RAISE(ABORT,'image is unavailable or belongs to another account') END;
 INSERT INTO asset_references SELECT 'field',NEW.id,'icon',a.id FROM uploaded_assets a WHERE instr(NEW.icon,'/api/assets/')>0 AND a.id=substr(NEW.icon,instr(NEW.icon,'/api/assets/')+12,36) AND a.user_id=(SELECT g.user_id FROM object_types t JOIN games g ON g.id=t.game_id WHERE t.id=NEW.object_type_id) AND a.deleted_at IS NULL;
END;

CREATE TRIGGER field_definitions_asset_refs_delete AFTER DELETE ON field_definitions
BEGIN DELETE FROM asset_references WHERE entity_type='field' AND entity_id=OLD.id; END;

INSERT INTO asset_references SELECT 'object',s.id,'image',a.id FROM objects s JOIN uploaded_assets a ON a.id=substr(s.image,instr(s.image,'/api/assets/')+12,36) AND a.user_id=(SELECT user_id FROM games WHERE id=s.game_id) WHERE instr(s.image,'/api/assets/')>0;

CREATE TRIGGER objects_asset_refs_insert AFTER INSERT ON objects
BEGIN
 DELETE FROM asset_references WHERE entity_type='object' AND entity_id=NEW.id;
 SELECT CASE WHEN substr(NEW.image,1,12)='/api/assets/' AND NOT EXISTS (SELECT 1 FROM uploaded_assets WHERE id=substr(NEW.image,instr(NEW.image,'/api/assets/')+12,36) AND user_id=(SELECT user_id FROM games WHERE id=NEW.game_id) AND deleted_at IS NULL) THEN RAISE(ABORT,'image is unavailable or belongs to another account') END;
 INSERT INTO asset_references SELECT 'object',NEW.id,'image',a.id FROM uploaded_assets a WHERE instr(NEW.image,'/api/assets/')>0 AND a.id=substr(NEW.image,instr(NEW.image,'/api/assets/')+12,36) AND a.user_id=(SELECT user_id FROM games WHERE id=NEW.game_id) AND a.deleted_at IS NULL;
END;

CREATE TRIGGER objects_asset_refs_update AFTER UPDATE ON objects
BEGIN
 DELETE FROM asset_references WHERE entity_type='object' AND entity_id=NEW.id;
 SELECT CASE WHEN substr(NEW.image,1,12)='/api/assets/' AND NOT EXISTS (SELECT 1 FROM uploaded_assets WHERE id=substr(NEW.image,instr(NEW.image,'/api/assets/')+12,36) AND user_id=(SELECT user_id FROM games WHERE id=NEW.game_id) AND deleted_at IS NULL) THEN RAISE(ABORT,'image is unavailable or belongs to another account') END;
 INSERT INTO asset_references SELECT 'object',NEW.id,'image',a.id FROM uploaded_assets a WHERE instr(NEW.image,'/api/assets/')>0 AND a.id=substr(NEW.image,instr(NEW.image,'/api/assets/')+12,36) AND a.user_id=(SELECT user_id FROM games WHERE id=NEW.game_id) AND a.deleted_at IS NULL;
END;

CREATE TRIGGER objects_asset_refs_delete AFTER DELETE ON objects
BEGIN DELETE FROM asset_references WHERE entity_type='object' AND entity_id=OLD.id; END;

CREATE TRIGGER assets_protect_references BEFORE UPDATE OF deleted_at ON uploaded_assets
WHEN NEW.deleted_at IS NOT NULL AND EXISTS (SELECT 1 FROM asset_references WHERE asset_id=OLD.id)
BEGIN SELECT RAISE(ABORT,'image is in use'); END;
