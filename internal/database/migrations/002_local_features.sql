ALTER TABLE object_types ADD COLUMN archived INTEGER NOT NULL DEFAULT 0 CHECK (archived IN (0,1));
ALTER TABLE games ADD COLUMN icon TEXT NOT NULL DEFAULT '🎮';

-- Older handlers accepted unused links on manual/numeric/checklist goals and
-- fields from the wrong object type. Retain the goals, removing invalid links.
UPDATE goals SET object_id=NULL,field_id=NULL WHERE goal_type<>'field_target';
UPDATE goals SET object_id=NULL,field_id=NULL
WHERE goal_type='field_target' AND NOT EXISTS (
    SELECT 1 FROM objects o JOIN field_definitions f ON f.object_type_id=o.object_type_id
    WHERE o.id=goals.object_id AND o.game_id=goals.game_id AND f.id=goals.field_id
);

CREATE TABLE section_goal_placements (
    section_id TEXT NOT NULL,
    game_id TEXT NOT NULL,
    goal_id TEXT NOT NULL REFERENCES goals(id) ON DELETE CASCADE,
    created_at TEXT NOT NULL,
    PRIMARY KEY(section_id, goal_id),
    FOREIGN KEY(section_id, game_id) REFERENCES sections(id, game_id) ON DELETE CASCADE
);
CREATE INDEX goal_placements_goal_idx ON section_goal_placements(goal_id);
CREATE TRIGGER goal_placement_game_insert BEFORE INSERT ON section_goal_placements
WHEN NOT EXISTS (SELECT 1 FROM goals WHERE id=NEW.goal_id AND game_id=NEW.game_id)
BEGIN SELECT RAISE(ABORT, 'goal must belong to section game'); END;

-- Assets belong to the account and can be reused across games. Preserve their
-- metadata and quota accounting when a game is deleted.
CREATE TRIGGER preserve_account_assets BEFORE DELETE ON games
BEGIN UPDATE uploaded_assets SET game_id=NULL WHERE game_id=OLD.id; END;
