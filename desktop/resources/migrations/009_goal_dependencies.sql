CREATE TABLE goal_dependencies (
    game_id TEXT NOT NULL REFERENCES games(id) ON DELETE CASCADE,
    goal_id TEXT NOT NULL REFERENCES goals(id) ON DELETE CASCADE,
    prerequisite_goal_id TEXT NOT NULL REFERENCES goals(id) ON DELETE CASCADE,
    created_at TEXT NOT NULL,
    PRIMARY KEY(goal_id, prerequisite_goal_id),
    CHECK (goal_id <> prerequisite_goal_id)
);
CREATE INDEX goal_dependencies_prerequisite_idx ON goal_dependencies(prerequisite_goal_id, goal_id);
CREATE TRIGGER goal_dependency_same_game_insert BEFORE INSERT ON goal_dependencies
WHEN NOT EXISTS (
    SELECT 1 FROM goals dependent JOIN goals prerequisite ON prerequisite.id=NEW.prerequisite_goal_id
    WHERE dependent.id=NEW.goal_id AND dependent.game_id=NEW.game_id AND prerequisite.game_id=NEW.game_id
)
BEGIN SELECT RAISE(ABORT, 'goal dependencies must stay within one game'); END;
