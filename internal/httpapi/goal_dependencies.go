package httpapi

import (
	"context"
	"database/sql"
	"encoding/json"
	"net/http"
	"sort"
	"strings"
	"time"
)

const maxGoalDependencies = 50

type goalDependencyStatus struct {
	ID        string  `json:"id"`
	Title     string  `json:"title"`
	Completed bool    `json:"completed"`
	Progress  float64 `json:"progress"`
}

func (a *API) getGoalDependencies(w http.ResponseWriter, r *http.Request) {
	item, err := a.loadGoal(r, r.PathValue("goalID"))
	if err != nil {
		notFoundOrError(w, err, "Goal")
		return
	}
	goals := []goal{item}
	if err := a.populateGoalDependencies(r.Context(), goals); err != nil {
		writeError(w, http.StatusInternalServerError, "database_error", "Could not load goal dependencies.")
		return
	}
	item = goals[0]
	writeJSON(w, http.StatusOK, map[string]any{"goal_id": item.ID, "ready": item.Ready, "blocked_by": item.BlockedBy, "items": item.Dependencies})
}

func (a *API) setGoalDependencies(w http.ResponseWriter, r *http.Request) {
	item, err := a.loadGoal(r, r.PathValue("goalID"))
	if err != nil {
		notFoundOrError(w, err, "Goal")
		return
	}
	var input struct {
		DependsOn []string `json:"depends_on_ids"`
	}
	if !decodeJSON(w, r, &input) {
		return
	}
	ids := uniqueSortedStrings(input.DependsOn)
	if len(ids) > maxGoalDependencies {
		writeError(w, http.StatusBadRequest, "too_many_dependencies", "A goal can have at most 50 prerequisites.")
		return
	}
	for _, id := range ids {
		if id == item.ID {
			writeError(w, http.StatusBadRequest, "dependency_cycle", "A goal cannot depend on itself.")
			return
		}
	}
	tx, err := a.db.BeginTx(r.Context(), nil)
	if err != nil {
		writeError(w, http.StatusInternalServerError, "dependency_failed", "Could not update goal dependencies.")
		return
	}
	defer tx.Rollback()
	for _, id := range ids {
		var exists int
		if err := tx.QueryRowContext(r.Context(), `SELECT COUNT(*) FROM goals WHERE id=? AND game_id=?`, id, item.GameID).Scan(&exists); err != nil {
			writeError(w, http.StatusInternalServerError, "database_error", "Could not validate prerequisite goals.")
			return
		}
		if exists != 1 {
			writeError(w, http.StatusBadRequest, "invalid_dependency", "Every prerequisite must be another goal in the same game.")
			return
		}
	}
	graph, err := loadGoalDependencyGraph(r.Context(), tx, item.GameID)
	if err != nil {
		writeError(w, http.StatusInternalServerError, "database_error", "Could not validate dependency cycles.")
		return
	}
	graph[item.ID] = ids
	if hasGoalDependencyCycle(graph) {
		writeError(w, http.StatusBadRequest, "dependency_cycle", "These prerequisites would create a dependency cycle.")
		return
	}
	if _, err := tx.ExecContext(r.Context(), `DELETE FROM goal_dependencies WHERE goal_id=?`, item.ID); err != nil {
		writeError(w, http.StatusInternalServerError, "dependency_failed", "Could not replace goal dependencies.")
		return
	}
	now := time.Now().UTC().Format(time.RFC3339Nano)
	for _, id := range ids {
		if _, err := tx.ExecContext(r.Context(), `INSERT INTO goal_dependencies(game_id,goal_id,prerequisite_goal_id,created_at) VALUES(?,?,?,?)`, item.GameID, item.ID, id, now); err != nil {
			writeError(w, http.StatusInternalServerError, "dependency_failed", "Could not save goal dependencies.")
			return
		}
	}
	if err := tx.Commit(); err != nil {
		writeError(w, http.StatusInternalServerError, "dependency_failed", "Could not save goal dependencies.")
		return
	}
	goals := []goal{item}
	if err := a.populateGoalDependencies(r.Context(), goals); err != nil {
		writeError(w, http.StatusInternalServerError, "database_error", "Dependencies were saved but their status could not be loaded.")
		return
	}
	writeJSON(w, http.StatusOK, map[string]any{"goal_id": item.ID, "ready": goals[0].Ready, "blocked_by": goals[0].BlockedBy, "items": goals[0].Dependencies})
}

func (a *API) populateGoalDependencies(ctx context.Context, goals []goal) error {
	for i := range goals {
		goals[i].Dependencies = []goalDependencyStatus{}
		goals[i].BlockedBy = []string{}
		goals[i].Ready = true
	}
	if len(goals) == 0 {
		return nil
	}
	indexes := make(map[string]int, len(goals))
	for i := range goals {
		indexes[goals[i].ID] = i
	}
	prerequisites := map[string]goal{}
	const batchSize = 300
	for start := 0; start < len(goals); start += batchSize {
		end := start + batchSize
		if end > len(goals) {
			end = len(goals)
		}
		batch := goals[start:end]
		placeholders := strings.TrimSuffix(strings.Repeat("?,", len(batch)), ",")
		args := make([]any, 0, len(batch))
		for _, item := range batch {
			args = append(args, item.ID)
		}
		rows, err := a.db.QueryContext(ctx, `SELECT d.goal_id,p.id,p.game_id,p.title,p.description,p.goal_type,p.object_id,p.field_id,p.target_json,p.current_value,p.completed,p.created_at,p.updated_at FROM goal_dependencies d JOIN goals p ON p.id=d.prerequisite_goal_id WHERE d.goal_id IN (`+placeholders+`) ORDER BY d.goal_id,p.sort_order,p.id`, args...)
		if err != nil {
			return err
		}
		for rows.Next() {
			var goalID string
			var item goal
			var objectID, fieldID sql.NullString
			var target []byte
			var completed int
			if err := rows.Scan(&goalID, &item.ID, &item.GameID, &item.Title, &item.Description, &item.GoalType, &objectID, &fieldID, &target, &item.Current, &completed, &item.CreatedAt, &item.UpdatedAt); err != nil {
				rows.Close()
				return err
			}
			if objectID.Valid {
				item.ObjectID = &objectID.String
			}
			if fieldID.Valid {
				item.FieldID = &fieldID.String
			}
			if len(target) > 0 {
				item.Target = append(json.RawMessage(nil), target...)
			}
			item.Completed = completed != 0
			if index, ok := indexes[goalID]; ok {
				goals[index].Dependencies = append(goals[index].Dependencies, goalDependencyStatus{ID: item.ID, Title: item.Title})
			}
			prerequisites[item.ID] = item
		}
		if err := rows.Err(); err != nil {
			rows.Close()
			return err
		}
		if err := rows.Close(); err != nil {
			return err
		}
	}
	ids := make([]string, 0, len(prerequisites))
	for id := range prerequisites {
		ids = append(ids, id)
	}
	sort.Strings(ids)
	if len(ids) == 0 {
		return nil
	}
	ctx = withReadCache(ctx)
	summaries, err := a.goalTaskSummaries(ctx, ids)
	if err != nil {
		return err
	}
	if cache, _ := ctx.Value(readCacheKey{}).(*readCache); cache != nil {
		cache.goalTaskSummaries = summaries
	}
	statuses := make(map[string]goalDependencyStatus, len(ids))
	for _, id := range ids {
		item := prerequisites[id]
		if err := a.populateGoalProgress(ctx, &item); err != nil {
			return err
		}
		statuses[id] = goalDependencyStatus{ID: item.ID, Title: item.Title, Completed: item.Completed, Progress: item.Progress}
	}
	for i := range goals {
		for j := range goals[i].Dependencies {
			status := statuses[goals[i].Dependencies[j].ID]
			goals[i].Dependencies[j] = status
			if !status.Completed {
				goals[i].Ready = false
				goals[i].BlockedBy = append(goals[i].BlockedBy, status.Title)
			}
		}
	}
	return nil
}

func loadGoalDependencyGraph(ctx context.Context, db dbExecutor, gameID string) (map[string][]string, error) {
	rows, err := db.QueryContext(ctx, `SELECT goal_id,prerequisite_goal_id FROM goal_dependencies WHERE game_id=? ORDER BY goal_id,prerequisite_goal_id`, gameID)
	if err != nil {
		return nil, err
	}
	graph := map[string][]string{}
	for rows.Next() {
		var goalID, dependencyID string
		if err := rows.Scan(&goalID, &dependencyID); err != nil {
			rows.Close()
			return nil, err
		}
		graph[goalID] = append(graph[goalID], dependencyID)
	}
	if err := rows.Err(); err != nil {
		rows.Close()
		return nil, err
	}
	return graph, rows.Close()
}

func hasGoalDependencyCycle(graph map[string][]string) bool {
	state := map[string]uint8{}
	var visit func(string) bool
	visit = func(id string) bool {
		if state[id] == 1 {
			return true
		}
		if state[id] == 2 {
			return false
		}
		state[id] = 1
		for _, dependency := range graph[id] {
			if visit(dependency) {
				return true
			}
		}
		state[id] = 2
		return false
	}
	for id := range graph {
		if visit(id) {
			return true
		}
	}
	return false
}

func uniqueSortedStrings(values []string) []string {
	seen := map[string]bool{}
	result := make([]string, 0, len(values))
	for _, value := range values {
		value = strings.TrimSpace(value)
		if value != "" && !seen[value] {
			seen[value] = true
			result = append(result, value)
		}
	}
	sort.Strings(result)
	return result
}

func goalHasReadyPrerequisites(ctx context.Context, a *API, item goal) (bool, []string, error) {
	items := []goal{item}
	if err := a.populateGoalDependencies(ctx, items); err != nil {
		return false, nil, err
	}
	return items[0].Ready, items[0].BlockedBy, nil
}
