-- Presentation references retain stable field IDs, including archived fields.
ALTER TABLE object_types ADD COLUMN primary_display_field_id TEXT NOT NULL DEFAULT '';
CREATE TRIGGER primary_display_field_insert BEFORE INSERT ON object_types
WHEN NEW.primary_display_field_id <> '' AND NOT EXISTS (
    SELECT 1 FROM field_definitions WHERE id=NEW.primary_display_field_id AND object_type_id=NEW.id
)
BEGIN SELECT RAISE(ABORT, 'primary display field must belong to object type'); END;
CREATE TRIGGER primary_display_field_update BEFORE UPDATE OF primary_display_field_id ON object_types
WHEN NEW.primary_display_field_id <> '' AND NOT EXISTS (
    SELECT 1 FROM field_definitions WHERE id=NEW.primary_display_field_id AND object_type_id=NEW.id
)
BEGIN SELECT RAISE(ABORT, 'primary display field must belong to object type'); END;
