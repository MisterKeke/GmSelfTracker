package httpapi

import (
	"encoding/json"
	"math"
	"net/http"
	"strings"
	"time"
)

// updateGoal retains goal/task identities and placements while editing one goal.
func (a *API) updateGoal(w http.ResponseWriter, r *http.Request) {
	item, err := a.loadGoal(r, r.PathValue("goalID"))
	if err != nil {
		notFoundOrError(w, err, "Goal")
		return
	}
	var input struct {
		Title       *string          `json:"title"`
		Description *string          `json:"description"`
		Completed   *bool            `json:"completed"`
		Current     *float64         `json:"current"`
		Target      json.RawMessage  `json:"target"`
		ObjectID    *string          `json:"object_id"`
		FieldID     *string          `json:"field_id"`
		Conditions  *[]goalCondition `json:"conditions"`
		Mode        *string          `json:"mode"`
		Tasks       *[]goalTask      `json:"tasks"`
	}
	if !decodeJSON(w, r, &input) {
		return
	}
	invalid := func(code, message string) { writeError(w, 400, code, message) }
	if input.Title != nil {
		item.Title = strings.TrimSpace(*input.Title)
		if item.Title == "" || len(item.Title) > 160 {
			invalid("invalid_title", "Goal title must contain 1–160 characters.")
			return
		}
	}
	if input.Description != nil {
		item.Description = *input.Description
	}
	if input.Completed != nil {
		if item.GoalType != "manual" {
			writeError(w, 409, "calculated_goal_read_only", "Completion is calculated for this goal type.")
			return
		}
		item.Completed = *input.Completed
	}
	if input.Current != nil {
		if item.GoalType != "numeric" {
			writeError(w, 409, "calculated_goal_read_only", "Only numeric goals accept manual progress.")
			return
		}
		if *input.Current < 0 || math.IsNaN(*input.Current) || math.IsInf(*input.Current, 0) {
			invalid("invalid_current", "Current progress must be a nonnegative finite number.")
			return
		}
		item.Current = *input.Current
	}
	hasRuleEdit := input.Conditions != nil || input.Mode != nil || input.ObjectID != nil || input.FieldID != nil
	if hasRuleEdit && item.GoalType != "field_target" {
		invalid("invalid_goal", "Only field-linked goals accept conditions or field references.")
		return
	}
	if input.Target != nil || hasRuleEdit {
		switch item.GoalType {
		case "numeric":
			target, e := numberFromJSON(input.Target)
			if e != nil || target <= 0 {
				invalid("invalid_target", "Numeric goal target must be greater than zero.")
				return
			}
			item.Target = input.Target
		case "field_target":
			var rule goalRule
			isRule := json.Unmarshal(item.Target, &rule) == nil && len(rule.Conditions) > 0
			if input.Conditions != nil {
				rule.Conditions = *input.Conditions
				isRule = true
			} else if !isRule {
				// Scalar legacy goals retain their IDs and become equivalent editable rules.
				if input.ObjectID != nil {
					item.ObjectID = input.ObjectID
				}
				if input.FieldID != nil {
					item.FieldID = input.FieldID
				}
				if input.Target != nil {
					item.Target = input.Target
				}
				if item.ObjectID == nil || item.FieldID == nil {
					invalid("invalid_goal", "Choose an object and field.")
					return
				}
				rule = goalRule{Mode: "all", Conditions: []goalCondition{{ObjectID: *item.ObjectID, FieldID: *item.FieldID, Operator: "gte", Value: item.Target}}}
			} else if input.Target != nil || input.ObjectID != nil || input.FieldID != nil {
				invalid("invalid_goal", "Edit the condition list for a rule goal.")
				return
			}
			if input.Mode != nil {
				rule.Mode = *input.Mode
			}
			if rule.Mode == "" {
				rule.Mode = "all"
			}
			if err := a.validateGoalRule(r.Context(), item.GameID, rule); err != nil {
				invalid("invalid_goal", err.Error())
				return
			}
			item.Target, _ = json.Marshal(rule)
			item.ObjectID = nil
			item.FieldID = nil
		default:
			invalid("invalid_target", "This goal type does not use a target.")
			return
		}
	}
	if input.Tasks != nil {
		if item.GoalType != "checklist" {
			invalid("invalid_tasks", "Only checklist goals accept tasks.")
			return
		}
		if len(*input.Tasks) == 0 || len(*input.Tasks) > 200 {
			invalid("invalid_tasks", "Use between 1 and 200 checklist tasks.")
			return
		}
		seen := map[string]bool{}
		for i := range *input.Tasks {
			task := &(*input.Tasks)[i]
			task.Title = strings.TrimSpace(task.Title)
			task.SortOrder = i
			if task.Title == "" || len(task.Title) > 160 || task.ID != "" && seen[task.ID] {
				invalid("invalid_tasks", "Task titles must contain 1–160 characters and task IDs must be unique.")
				return
			}
			if task.ID != "" {
				seen[task.ID] = true
			}
		}
	}
	willComplete := item.GoalType == "manual" && item.Completed
	if item.GoalType == "numeric" {
		if target, e := numberFromJSON(item.Target); e == nil && item.Current >= target {
			willComplete = true
		}
	}
	if item.GoalType == "checklist" && input.Tasks != nil {
		willComplete = len(*input.Tasks) > 0
		for _, task := range *input.Tasks {
			willComplete = willComplete && task.Completed
		}
	}
	if willComplete {
		ready, blockedBy, err := goalHasReadyPrerequisites(r.Context(), a, item)
		if err != nil {
			writeError(w, 500, "database_error", "Could not validate goal readiness.")
			return
		}
		if !ready {
			writeJSON(w, 409, map[string]any{"error": map[string]string{"code": "goal_blocked", "message": "Complete the prerequisite goals before completing this goal."}, "blocked_by": blockedBy})
			return
		}
	}
	tx, err := a.db.BeginTx(r.Context(), nil)
	if err != nil {
		writeError(w, 500, "update_failed", "Could not update goal.")
		return
	}
	defer tx.Rollback()
	if input.Tasks != nil {
		rows, e := tx.QueryContext(r.Context(), `SELECT id FROM goal_tasks WHERE goal_id=?`, item.ID)
		if e != nil {
			writeError(w, 500, "update_failed", "Could not load checklist tasks.")
			return
		}
		existing := map[string]bool{}
		for rows.Next() {
			var id string
			if e = rows.Scan(&id); e != nil {
				break
			}
			existing[id] = true
		}
		if e == nil {
			e = rows.Err()
		}
		rows.Close()
		if e != nil {
			writeError(w, 500, "update_failed", "Could not load checklist tasks.")
			return
		}
		kept := map[string]bool{}
		for _, task := range *input.Tasks {
			if task.ID != "" && !existing[task.ID] {
				invalid("invalid_tasks", "Task IDs must belong to this goal.")
				return
			}
			if task.ID == "" {
				task.ID, e = randomID()
				if e == nil {
					_, e = tx.ExecContext(r.Context(), `INSERT INTO goal_tasks(id,goal_id,title,completed,sort_order,created_at) VALUES(?,?,?,?,?,?)`, task.ID, item.ID, task.Title, boolInt(task.Completed), task.SortOrder, time.Now().UTC().Format(time.RFC3339))
				}
			} else {
				_, e = tx.ExecContext(r.Context(), `UPDATE goal_tasks SET title=?,completed=?,sort_order=? WHERE id=? AND goal_id=?`, task.Title, boolInt(task.Completed), task.SortOrder, task.ID, item.ID)
			}
			if e != nil {
				writeError(w, 500, "update_failed", "Could not save checklist tasks.")
				return
			}
			kept[task.ID] = true
		}
		for id := range existing {
			if !kept[id] {
				if _, e = tx.ExecContext(r.Context(), `DELETE FROM goal_tasks WHERE id=? AND goal_id=?`, id, item.ID); e != nil {
					writeError(w, 500, "update_failed", "Could not remove checklist task.")
					return
				}
			}
		}
	}
	item.UpdatedAt = time.Now().UTC().Format(time.RFC3339)
	var target any
	if len(item.Target) > 0 {
		target = string(item.Target)
	}
	_, err = tx.ExecContext(r.Context(), `UPDATE goals SET title=?,description=?,completed=?,current_value=?,object_id=?,field_id=?,target_json=?,updated_at=? WHERE id=?`, item.Title, item.Description, boolInt(item.Completed), item.Current, nullableString(item.ObjectID), nullableString(item.FieldID), target, item.UpdatedAt, item.ID)
	if err != nil {
		writeError(w, 500, "update_failed", "Could not save goal changes.")
		return
	}
	if err = tx.Commit(); err != nil {
		writeError(w, 500, "update_failed", "Could not save goal changes.")
		return
	}
	item.SectionIDs, err = a.goalSections(r, item.ID)
	if err != nil {
		writeError(w, 500, "database_error", "Goal saved but placements could not be read.")
		return
	}
	if err = a.populateGoalProgress(r.Context(), &item); err != nil {
		writeError(w, 500, "database_error", "Goal saved but progress could not be calculated.")
		return
	}
	dependencyItems := []goal{item}
	if err = a.populateGoalDependencies(r.Context(), dependencyItems); err != nil {
		writeError(w, 500, "database_error", "Goal saved but dependencies could not be loaded.")
		return
	}
	item = dependencyItems[0]
	writeJSON(w, 200, item)
}
