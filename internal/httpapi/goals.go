package httpapi

import (
	"context"
	"database/sql"
	"encoding/json"
	"math"
	"net/http"
	"sort"
	"strconv"
	"strings"
	"time"
)

type goal struct {
	SectionIDs       []string              `json:"section_ids"`
	ID               string                `json:"id"`
	GameID           string                `json:"game_id"`
	Title            string                `json:"title"`
	Description      string                `json:"description"`
	GoalType         string                `json:"goal_type"`
	ObjectID         *string               `json:"object_id"`
	FieldID          *string               `json:"field_id"`
	Target           json.RawMessage       `json:"target,omitempty"`
	Current          float64               `json:"current"`
	Completed        bool                  `json:"completed"`
	Progress         float64               `json:"progress"`
	Tasks            []goalTask            `json:"tasks,omitempty"`
	ConditionDetails []goalConditionStatus `json:"condition_details,omitempty"`
	CreatedAt        string                `json:"created_at"`
	UpdatedAt        string                `json:"updated_at"`
}

type goalConditionStatus struct {
	ObjectID     string          `json:"object_id"`
	ObjectName   string          `json:"object_name,omitempty"`
	FieldID      string          `json:"field_id"`
	FieldName    string          `json:"field_name,omitempty"`
	Operator     string          `json:"operator"`
	Target       json.RawMessage `json:"target"`
	CurrentValue json.RawMessage `json:"current_value,omitempty"`
	Error        string          `json:"error,omitempty"`
	Matched      bool            `json:"matched"`
	State        string          `json:"state"`
}

type goalTask struct {
	ID        string `json:"id"`
	Title     string `json:"title"`
	Completed bool   `json:"completed"`
	SortOrder int    `json:"sort_order"`
}

func (a *API) listGoals(w http.ResponseWriter, r *http.Request) {
	r = r.WithContext(withReadCache(r.Context()))
	gameID := r.PathValue("gameID")
	if _, err := a.loadGame(r, gameID, false); err != nil {
		notFoundOrError(w, err, "Game")
		return
	}
	sectionID := r.URL.Query().Get("section_id")
	excludeSectionID := r.URL.Query().Get("exclude_section_id")
	for _, id := range []string{sectionID, excludeSectionID} {
		if id == "" {
			continue
		}
		if _, err := a.loadSection(r, id, false); err != nil {
			notFoundOrError(w, err, "Section")
			return
		}
		var matches int
		if err := a.db.QueryRowContext(r.Context(), `SELECT COUNT(*) FROM sections WHERE id=? AND game_id=? AND archived=0`, id, gameID).Scan(&matches); err != nil || matches != 1 {
			writeError(w, http.StatusNotFound, "not_found", "Section not found.")
			return
		}
	}
	query := `SELECT id,game_id,title,description,goal_type,object_id,field_id,target_json,current_value,completed,created_at,updated_at FROM goals WHERE game_id=?`
	args := []any{gameID}
	if sectionID != "" {
		query += ` AND EXISTS (SELECT 1 FROM section_goal_placements p WHERE p.goal_id=goals.id AND p.section_id=?)`
		args = append(args, sectionID)
	}
	if excludeSectionID != "" {
		query += ` AND NOT EXISTS (SELECT 1 FROM section_goal_placements p WHERE p.goal_id=goals.id AND p.section_id=?)`
		args = append(args, excludeSectionID)
	}
	query += ` ORDER BY sort_order,created_at`
	rows, err := a.db.QueryContext(r.Context(), query, args...)
	if err != nil {
		writeError(w, http.StatusInternalServerError, "database_error", "Could not load goals.")
		return
	}
	defer rows.Close()
	items := make([]goal, 0)
	for rows.Next() {
		var item goal
		var objectID, fieldID sql.NullString
		var target []byte
		var completed int
		if err := rows.Scan(&item.ID, &item.GameID, &item.Title, &item.Description, &item.GoalType, &objectID, &fieldID, &target, &item.Current, &completed, &item.CreatedAt, &item.UpdatedAt); err != nil {
			writeError(w, http.StatusInternalServerError, "database_error", "Could not load goals.")
			return
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
		items = append(items, item)
	}
	if err := rows.Err(); err != nil {
		writeError(w, http.StatusInternalServerError, "database_error", "Could not load goals.")
		return
	}
	if err := rows.Close(); err != nil {
		writeError(w, http.StatusInternalServerError, "database_error", "Could not load goals.")
		return
	}
	for i := range items {
		items[i].SectionIDs, err = a.goalSections(r, items[i].ID)
		if err != nil {
			writeError(w, 500, "database_error", "Could not load goal sections.")
			return
		}
		if err := a.populateGoalProgress(r.Context(), &items[i]); err != nil {
			writeError(w, http.StatusInternalServerError, "database_error", "Could not calculate goal progress.")
			return
		}
	}
	gameTotal := 0
	if err := a.db.QueryRowContext(r.Context(), `SELECT COUNT(*) FROM goals WHERE game_id=?`, gameID).Scan(&gameTotal); err != nil {
		writeError(w, http.StatusInternalServerError, "database_error", "Could not count goals.")
		return
	}
	completedTotal := 0
	for _, item := range items {
		if item.Completed {
			completedTotal++
		}
	}
	search := strings.ToLower(strings.TrimSpace(r.URL.Query().Get("search")))
	if len(search) > 2000 {
		writeError(w, http.StatusBadRequest, "invalid_search", "Goal search must be 2000 characters or fewer.")
		return
	}
	status := r.URL.Query().Get("status")
	goalType := r.URL.Query().Get("type")
	sortBy := r.URL.Query().Get("sort")
	direction := strings.ToLower(r.URL.Query().Get("direction"))
	if direction == "" && sortBy != "" {
		direction = "desc"
	}
	if status != "" && status != "all" && status != "active" && status != "completed" {
		writeError(w, http.StatusBadRequest, "invalid_query", "Choose all, active, or completed goal status.")
		return
	}
	if goalType != "" && goalType != "all" && goalType != "manual" && goalType != "checklist" && goalType != "numeric" && goalType != "field_linked" && goalType != "field_target" {
		writeError(w, http.StatusBadRequest, "invalid_query", "Choose a supported goal type.")
		return
	}
	if sortBy != "" && sortBy != "title" && sortBy != "created_at" && sortBy != "updated_at" && sortBy != "progress" && sortBy != "completed" {
		writeError(w, http.StatusBadRequest, "invalid_query", "Choose title, created date, updated date, progress, or completion status for goal sorting.")
		return
	}
	if direction != "" && direction != "asc" && direction != "desc" {
		writeError(w, http.StatusBadRequest, "invalid_query", "Choose ascending or descending goal order.")
		return
	}
	limit := len(items) + 1
	if r.URL.Query().Has("limit") {
		limit = queryInt(r, "limit", 20, 1, 100)
	}
	offset := queryInt(r, "offset", 0, 0, 1000000)
	filtered := make([]goal, 0, len(items))
	for _, item := range items {
		if search != "" && !strings.Contains(strings.ToLower(item.Title+" "+item.Description), search) {
			continue
		}
		if status == "completed" && !item.Completed || status == "active" && item.Completed {
			continue
		}
		if goalType != "" && goalType != "all" && !(goalType == "field_linked" && item.GoalType == "field_target") && goalType != item.GoalType {
			continue
		}
		filtered = append(filtered, item)
	}
	directionFactor := 1
	if direction == "desc" {
		directionFactor = -1
	}
	if sortBy != "" {
		sort.SliceStable(filtered, func(i, j int) bool {
			left, right := filtered[i], filtered[j]
			comparison := 0
			switch sortBy {
			case "title":
				comparison = strings.Compare(strings.ToLower(left.Title), strings.ToLower(right.Title))
			case "created_at":
				comparison = strings.Compare(left.CreatedAt, right.CreatedAt)
			case "updated_at":
				comparison = strings.Compare(left.UpdatedAt, right.UpdatedAt)
			case "progress":
				if left.Progress < right.Progress {
					comparison = -1
				} else if left.Progress > right.Progress {
					comparison = 1
				}
			case "completed":
				if !left.Completed && right.Completed {
					comparison = -1
				} else if left.Completed && !right.Completed {
					comparison = 1
				}
			}
			if comparison == 0 {
				return left.ID < right.ID
			}
			return comparison*directionFactor < 0
		})
	}
	total := len(filtered)
	if offset >= total {
		filtered = []goal{}
	} else {
		end := offset + limit
		if end > total {
			end = total
		}
		filtered = filtered[offset:end]
	}
	writeJSON(w, http.StatusOK, map[string]any{"items": filtered, "total": total, "game_total": gameTotal, "completed_total": completedTotal, "limit": limit, "offset": offset, "has_more": offset+len(filtered) < total})
}

func (a *API) getGoal(w http.ResponseWriter, r *http.Request) {
	r = r.WithContext(withReadCache(r.Context()))
	item, err := a.loadGoal(r, r.PathValue("goalID"))
	if err != nil {
		notFoundOrError(w, err, "Goal")
		return
	}
	item.SectionIDs, err = a.goalSections(r, item.ID)
	if err != nil {
		writeError(w, http.StatusInternalServerError, "database_error", "Could not load goal sections.")
		return
	}
	if err := a.populateGoalProgress(r.Context(), &item); err != nil {
		writeError(w, http.StatusInternalServerError, "database_error", "Could not calculate goal progress.")
		return
	}
	writeJSON(w, http.StatusOK, item)
}

func (a *API) createGoal(w http.ResponseWriter, r *http.Request) {
	gameID := r.PathValue("gameID")
	if _, err := a.loadGame(r, gameID, false); err != nil {
		notFoundOrError(w, err, "Game")
		return
	}
	var input struct {
		Title       string          `json:"title"`
		Description string          `json:"description"`
		GoalType    string          `json:"goal_type"`
		ObjectID    *string         `json:"object_id"`
		FieldID     *string         `json:"field_id"`
		Target      json.RawMessage `json:"target"`
		Current     float64         `json:"current"`
		Tasks       []string        `json:"tasks"`
		TaskDetails []goalTask      `json:"task_details"`
		Completed   *bool           `json:"completed"`
		Conditions  []goalCondition `json:"conditions"`
		Mode        string          `json:"mode"`
	}
	if !decodeJSON(w, r, &input) {
		return
	}
	if input.Completed != nil && input.GoalType != "manual" {
		writeError(w, 400, "invalid_goal", "Only manual goals accept a completion state.")
		return
	}
	if input.TaskDetails != nil {
		if input.GoalType != "checklist" || len(input.Tasks) > 0 {
			writeError(w, 400, "invalid_tasks", "Use task details only for a checklist, without a separate task list.")
			return
		}
		for _, task := range input.TaskDetails {
			if task.ID != "" {
				writeError(w, 400, "invalid_tasks", "New tasks cannot supply existing IDs.")
				return
			}
			input.Tasks = append(input.Tasks, task.Title)
		}
	}
	input.Title = strings.TrimSpace(input.Title)
	if input.Title == "" || len(input.Title) > 160 {
		writeError(w, http.StatusBadRequest, "invalid_title", "Goal title must be between 1 and 160 characters.")
		return
	}
	if input.GoalType != "manual" && input.GoalType != "checklist" && input.GoalType != "field_target" && input.GoalType != "numeric" {
		writeError(w, http.StatusBadRequest, "invalid_goal_type", "Choose manual, checklist, field target, or numeric progress.")
		return
	}
	if len(input.Conditions) > 0 {
		if input.GoalType != "field_target" {
			writeError(w, 400, "invalid_goal", "Conditions require a field target goal.")
			return
		}
		if input.Mode == "" {
			input.Mode = "all"
		}
		rule := goalRule{Mode: input.Mode, Conditions: input.Conditions}
		if err := a.validateGoalRule(r.Context(), gameID, rule); err != nil {
			writeError(w, 400, "invalid_goal", err.Error())
			return
		}
		input.Target, _ = json.Marshal(rule)
		input.ObjectID, input.FieldID = nil, nil
	}
	if input.GoalType != "field_target" {
		input.ObjectID, input.FieldID = nil, nil
	}
	if input.GoalType != "checklist" {
		input.Tasks = nil
	}
	if len(input.Tasks) > 200 {
		writeError(w, 400, "invalid_tasks", "Use no more than 200 tasks.")
		return
	}
	for i, title := range input.Tasks {
		input.Tasks[i] = strings.TrimSpace(title)
		if input.Tasks[i] == "" || len(input.Tasks[i]) > 160 {
			writeError(w, 400, "invalid_tasks", "Task titles must contain 1–160 characters.")
			return
		}
	}
	if input.GoalType == "field_target" && len(input.Conditions) == 0 {
		if input.ObjectID == nil || input.FieldID == nil || len(input.Target) == 0 {
			writeError(w, http.StatusBadRequest, "invalid_goal", "Field target goals need an object, a numeric field, and a target.")
			return
		}
		var fieldType, objectType, objectGame string
		err := a.db.QueryRowContext(r.Context(), `SELECT f.field_type,o.object_type_id,o.game_id FROM field_definitions f JOIN objects o ON o.id=? JOIN object_types t ON t.id=f.object_type_id WHERE f.id=? AND f.object_type_id=o.object_type_id AND t.game_id=o.game_id AND f.archived=0 AND o.archived=0 AND t.archived=0`, *input.ObjectID, *input.FieldID).Scan(&fieldType, &objectType, &objectGame)
		if err != nil || objectGame != gameID || !(fieldType == "integer" || fieldType == "decimal" || fieldType == "percentage" || fieldType == "progress" || fieldType == "formula") {
			writeError(w, http.StatusBadRequest, "invalid_goal_field", "Choose a numeric field on an object in this game.")
			return
		}
		_ = objectType
		if _, err := numberFromJSON(input.Target); err != nil {
			writeError(w, http.StatusBadRequest, "invalid_target", "Goal target must be a number.")
			return
		}
	}
	if input.GoalType == "numeric" {
		if len(input.Target) == 0 {
			writeError(w, http.StatusBadRequest, "invalid_target", "Numeric goals need a target value.")
			return
		}
		target, err := numberFromJSON(input.Target)
		if err != nil || target <= 0 {
			writeError(w, http.StatusBadRequest, "invalid_target", "Numeric goal target must be greater than zero.")
			return
		}
		if input.Current < 0 || math.IsNaN(input.Current) || math.IsInf(input.Current, 0) {
			writeError(w, http.StatusBadRequest, "invalid_current", "Current progress must be a nonnegative number.")
			return
		}
	}
	if input.GoalType == "checklist" && len(input.Tasks) == 0 {
		writeError(w, http.StatusBadRequest, "empty_checklist", "Add at least one task to a checklist goal.")
		return
	}
	id, err := randomID()
	if err != nil {
		writeError(w, http.StatusInternalServerError, "create_failed", "Could not create goal.")
		return
	}
	now := time.Now().UTC().Format(time.RFC3339)
	var target any
	if len(input.Target) > 0 {
		if !json.Valid(input.Target) {
			writeError(w, http.StatusBadRequest, "invalid_target", "Target must be valid JSON.")
			return
		}
		target = string(input.Target)
	}
	tx, err := a.db.BeginTx(r.Context(), nil)
	if err != nil {
		writeError(w, http.StatusInternalServerError, "create_failed", "Could not create goal.")
		return
	}
	defer tx.Rollback()
	_, err = tx.ExecContext(r.Context(), `INSERT INTO goals(id,game_id,title,description,goal_type,object_id,field_id,target_json,current_value,completed,created_at,updated_at) VALUES(?,?,?,?,?,?,?,?,?,?,?,?)`, id, gameID, input.Title, input.Description, input.GoalType, nullableString(input.ObjectID), nullableString(input.FieldID), target, input.Current, boolInt(input.Completed != nil && *input.Completed), now, now)
	if err != nil {
		writeError(w, http.StatusInternalServerError, "create_failed", "Could not create goal.")
		return
	}
	for i, title := range input.Tasks {
		title = strings.TrimSpace(title)
		if title == "" {
			continue
		}
		taskID, err := randomID()
		if err != nil {
			writeError(w, http.StatusInternalServerError, "create_failed", "Could not create checklist.")
			return
		}
		if _, err := tx.ExecContext(r.Context(), `INSERT INTO goal_tasks(id,goal_id,title,completed,sort_order,created_at) VALUES(?,?,?,?,?,?)`, taskID, id, title, boolInt(input.TaskDetails != nil && input.TaskDetails[i].Completed), i, now); err != nil {
			writeError(w, http.StatusInternalServerError, "create_failed", "Could not create checklist.")
			return
		}
	}
	if err := tx.Commit(); err != nil {
		writeError(w, http.StatusInternalServerError, "create_failed", "Could not create goal.")
		return
	}
	item := goal{ID: id, GameID: gameID, Title: input.Title, Description: input.Description, GoalType: input.GoalType, ObjectID: input.ObjectID, FieldID: input.FieldID, Target: input.Target, Current: input.Current, Completed: input.Completed != nil && *input.Completed, CreatedAt: now, UpdatedAt: now}
	if err := a.populateGoalProgress(r.Context(), &item); err != nil {
		writeError(w, http.StatusInternalServerError, "database_error", "Goal saved but progress could not be calculated.")
		return
	}
	writeJSON(w, http.StatusCreated, item)
}

func (a *API) deleteGoal(w http.ResponseWriter, r *http.Request) {
	item, err := a.loadGoal(r, r.PathValue("goalID"))
	if err != nil {
		notFoundOrError(w, err, "Goal")
		return
	}
	if _, err := a.db.ExecContext(r.Context(), `DELETE FROM goals WHERE id=?`, item.ID); err != nil {
		writeError(w, http.StatusInternalServerError, "delete_failed", "Could not delete goal.")
		return
	}
	w.WriteHeader(http.StatusNoContent)
}

func (a *API) updateGoalTask(w http.ResponseWriter, r *http.Request) {
	var input struct {
		Title     *string `json:"title"`
		Completed *bool   `json:"completed"`
	}
	if !decodeJSON(w, r, &input) {
		return
	}
	var task goalTask
	var gameOwner string
	err := a.db.QueryRowContext(r.Context(), `SELECT t.id,t.title,t.completed,t.sort_order,g.user_id FROM goal_tasks t JOIN goals gl ON gl.id=t.goal_id JOIN games g ON g.id=gl.game_id WHERE t.id=?`, r.PathValue("taskID")).Scan(&task.ID, &task.Title, &task.Completed, &task.SortOrder, &gameOwner)
	if err != nil || gameOwner != a.currentUser(r).ID {
		writeError(w, http.StatusNotFound, "not_found", "Checklist task not found.")
		return
	}
	if input.Title != nil {
		task.Title = strings.TrimSpace(*input.Title)
		if task.Title == "" || len(task.Title) > 160 {
			writeError(w, http.StatusBadRequest, "invalid_title", "Task title must be between 1 and 160 characters.")
			return
		}
	}
	if input.Completed != nil {
		task.Completed = *input.Completed
	}
	_, err = a.db.ExecContext(r.Context(), `UPDATE goal_tasks SET title=?,completed=? WHERE id=?`, task.Title, boolInt(task.Completed), task.ID)
	if err != nil {
		writeError(w, http.StatusInternalServerError, "update_failed", "Could not update checklist task.")
		return
	}
	writeJSON(w, http.StatusOK, task)
}

func (a *API) createGoalTask(w http.ResponseWriter, r *http.Request) {
	goalID := r.PathValue("goalID")
	item, err := a.loadGoal(r, goalID)
	if err != nil {
		notFoundOrError(w, err, "Goal")
		return
	}
	if item.GoalType != "checklist" {
		writeError(w, http.StatusConflict, "not_checklist", "Tasks can only be added to checklist goals.")
		return
	}
	var input struct {
		Title string `json:"title"`
	}
	if !decodeJSON(w, r, &input) {
		return
	}
	input.Title = strings.TrimSpace(input.Title)
	if input.Title == "" || len(input.Title) > 160 {
		writeError(w, http.StatusBadRequest, "invalid_title", "Task title must be between 1 and 160 characters.")
		return
	}
	var order int
	if err := a.db.QueryRowContext(r.Context(), `SELECT COALESCE(MAX(sort_order)+1,0) FROM goal_tasks WHERE goal_id=?`, goalID).Scan(&order); err != nil {
		writeError(w, http.StatusInternalServerError, "database_error", "Could not add checklist task.")
		return
	}
	id, err := randomID()
	if err != nil {
		writeError(w, http.StatusInternalServerError, "create_failed", "Could not add checklist task.")
		return
	}
	_, err = a.db.ExecContext(r.Context(), `INSERT INTO goal_tasks(id,goal_id,title,sort_order,created_at) VALUES(?,?,?,?,?)`, id, goalID, input.Title, order, time.Now().UTC().Format(time.RFC3339))
	if err != nil {
		writeError(w, http.StatusInternalServerError, "create_failed", "Could not add checklist task.")
		return
	}
	writeJSON(w, http.StatusCreated, goalTask{ID: id, Title: input.Title, SortOrder: order})
}

func (a *API) deleteGoalTask(w http.ResponseWriter, r *http.Request) {
	taskID := r.PathValue("taskID")
	var owner string
	err := a.db.QueryRowContext(r.Context(), `SELECT g.user_id FROM goal_tasks t JOIN goals gl ON gl.id=t.goal_id JOIN games g ON g.id=gl.game_id WHERE t.id=?`, taskID).Scan(&owner)
	if err != nil || owner != a.currentUser(r).ID {
		writeError(w, http.StatusNotFound, "not_found", "Checklist task not found.")
		return
	}
	if _, err := a.db.ExecContext(r.Context(), `DELETE FROM goal_tasks WHERE id=?`, taskID); err != nil {
		writeError(w, http.StatusInternalServerError, "delete_failed", "Could not delete checklist task.")
		return
	}
	w.WriteHeader(http.StatusNoContent)
}

func (a *API) loadGoal(r *http.Request, id string) (goal, error) {
	var item goal
	var objectID, fieldID sql.NullString
	var target []byte
	var completed int
	err := a.db.QueryRowContext(r.Context(), `SELECT gl.id,gl.game_id,gl.title,gl.description,gl.goal_type,gl.object_id,gl.field_id,gl.target_json,gl.current_value,gl.completed,gl.created_at,gl.updated_at FROM goals gl JOIN games g ON g.id=gl.game_id WHERE gl.id=? AND g.user_id=?`, id, a.currentUser(r).ID).Scan(&item.ID, &item.GameID, &item.Title, &item.Description, &item.GoalType, &objectID, &fieldID, &target, &item.Current, &completed, &item.CreatedAt, &item.UpdatedAt)
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
	return item, err
}

func (a *API) populateGoalProgress(ctx context.Context, item *goal) error {
	if ctx.Value(readCacheKey{}) == nil {
		ctx = withReadCache(ctx)
	}
	switch item.GoalType {
	case "manual":
		if item.Completed {
			item.Progress = 100
		} else {
			item.Progress = 0
		}
	case "numeric":
		target, err := numberFromJSON(item.Target)
		if err != nil || target <= 0 {
			return nil
		}
		item.Progress = math.Min(100, item.Current/target*100)
		item.Completed = item.Current >= target
	case "checklist":
		rows, err := a.db.QueryContext(ctx, `SELECT id,title,completed,sort_order FROM goal_tasks WHERE goal_id=? ORDER BY sort_order`, item.ID)
		if err != nil {
			return err
		}
		item.Tasks = make([]goalTask, 0)
		done := 0
		for rows.Next() {
			var task goalTask
			var complete int
			if err := rows.Scan(&task.ID, &task.Title, &complete, &task.SortOrder); err != nil {
				rows.Close()
				return err
			}
			task.Completed = complete != 0
			if task.Completed {
				done++
			}
			item.Tasks = append(item.Tasks, task)
		}
		if err := rows.Close(); err != nil {
			return err
		}
		if len(item.Tasks) > 0 {
			item.Progress = float64(done) / float64(len(item.Tasks)) * 100
			item.Completed = done == len(item.Tasks)
		}
	case "field_target":
		var rule goalRule
		if json.Unmarshal(item.Target, &rule) == nil && len(rule.Conditions) > 0 {
			return a.populateRuleProgress(ctx, item, rule)
		}
		if item.ObjectID == nil || item.FieldID == nil {
			return nil
		}
		record, err := a.goalObject(ctx, item.GameID, *item.ObjectID)
		if err == sql.ErrNoRows {
			return nil
		}
		if err != nil {
			return err
		}
		values := record.Values
		raw, ok := values[*item.FieldID]
		if !ok {
			return nil
		}
		current, e := numberFromJSON(raw)
		if e != nil {
			var progress struct {
				Current json.Number `json:"current"`
			}
			if json.Unmarshal(raw, &progress) == nil {
				current, _ = strconv.ParseFloat(string(progress.Current), 64)
				e = nil
			}
		}
		if e == nil {
			target, _ := numberFromJSON(item.Target)
			item.Current = current
			if target > 0 {
				item.Progress = math.Max(0, math.Min(100, current/target*100))
				item.Completed = current >= target
			} else {
				item.Completed = current >= target
				if item.Completed {
					item.Progress = 100
				}
			}
		}
	}
	return nil
}

func numberFromJSON(raw []byte) (float64, error) {
	var value any
	decoder := json.NewDecoder(strings.NewReader(string(raw)))
	decoder.UseNumber()
	if err := decoder.Decode(&value); err != nil {
		return 0, err
	}
	n, ok := value.(json.Number)
	if !ok {
		return 0, errText("Enter a number.")
	}
	number, err := strconv.ParseFloat(string(n), 64)
	if err != nil || math.IsNaN(number) || math.IsInf(number, 0) {
		return 0, errText("Enter a finite number.")
	}
	return number, nil
}
