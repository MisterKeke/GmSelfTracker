package httpapi

import (
	"encoding/json"
	"net/http"
	"strings"
	"time"
)

func (a *API) validIcon(r *http.Request, value string) bool {
	if strings.HasPrefix(value, "/") || strings.Contains(value, ":") {
		return a.validateImage(r, value)
	}
	return len(value) <= 64
}

func (a *API) archiveObjectType(w http.ResponseWriter, r *http.Request) {
	a.setObjectTypeArchived(w, r, true)
}
func (a *API) restoreObjectType(w http.ResponseWriter, r *http.Request) {
	a.setObjectTypeArchived(w, r, false)
}
func (a *API) setObjectTypeArchived(w http.ResponseWriter, r *http.Request, archived bool) {
	item, err := a.loadObjectType(r, r.PathValue("typeID"))
	if err != nil {
		notFoundOrError(w, err, "Object type")
		return
	}
	if _, err := a.db.ExecContext(r.Context(), `UPDATE object_types SET archived=?,updated_at=? WHERE id=?`, boolInt(archived), time.Now().UTC().Format(time.RFC3339), item.ID); err != nil {
		writeError(w, 500, "update_failed", "Could not update object type.")
		return
	}
	w.WriteHeader(http.StatusNoContent)
}

func (a *API) addGoalPlacement(w http.ResponseWriter, r *http.Request) {
	section, err := a.loadSection(r, r.PathValue("sectionID"), false)
	if err != nil {
		notFoundOrError(w, err, "Section")
		return
	}
	var input struct {
		GoalID string `json:"goal_id"`
	}
	if !decodeJSON(w, r, &input) {
		return
	}
	goal, err := a.loadGoal(r, input.GoalID)
	if err != nil || goal.GameID != section.GameID {
		writeError(w, 400, "invalid_goal", "Choose a goal from this game.")
		return
	}
	_, err = a.db.ExecContext(r.Context(), `INSERT INTO section_goal_placements(section_id,game_id,goal_id,created_at) VALUES(?,?,?,?) ON CONFLICT DO NOTHING`, section.ID, section.GameID, goal.ID, time.Now().UTC().Format(time.RFC3339))
	if err != nil {
		writeError(w, 500, "placement_failed", "Could not place goal.")
		return
	}
	w.WriteHeader(http.StatusNoContent)
}

func (a *API) removeGoalPlacement(w http.ResponseWriter, r *http.Request) {
	section, err := a.loadSection(r, r.PathValue("sectionID"), false)
	if err != nil {
		notFoundOrError(w, err, "Section")
		return
	}
	if _, err := a.db.ExecContext(r.Context(), `DELETE FROM section_goal_placements WHERE section_id=? AND goal_id=?`, section.ID, r.PathValue("goalID")); err != nil {
		writeError(w, 500, "delete_failed", "Could not remove goal from section.")
		return
	}
	w.WriteHeader(http.StatusNoContent)
}

func (a *API) goalSections(r *http.Request, id string) ([]string, error) {
	rows, err := a.db.QueryContext(r.Context(), `SELECT p.section_id FROM section_goal_placements p JOIN sections s ON s.id=p.section_id WHERE p.goal_id=? AND s.archived=0 ORDER BY s.sort_order,s.id`, id)
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	ids := []string{}
	for rows.Next() {
		var id string
		if err := rows.Scan(&id); err != nil {
			return nil, err
		}
		ids = append(ids, id)
	}
	return ids, rows.Err()
}

type goalCondition struct {
	ObjectID string          `json:"object_id"`
	FieldID  string          `json:"field_id"`
	Operator string          `json:"operator"`
	Value    json.RawMessage `json:"value"`
}

type goalRule struct {
	Mode       string          `json:"mode"`
	Conditions []goalCondition `json:"conditions"`
}
