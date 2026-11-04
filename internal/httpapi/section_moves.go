package httpapi

import (
	"net/http"
	"time"
)

// moveSection changes parent and sibling order together, leaving placements intact.
func (a *API) moveSection(w http.ResponseWriter, r *http.Request) {
	item, err := a.loadSection(r, r.PathValue("sectionID"), false)
	if err != nil {
		notFoundOrError(w, err, "Section")
		return
	}
	var input struct {
		ParentID *string `json:"parent_id"`
		BeforeID string  `json:"before_id"`
	}
	if !decodeJSON(w, r, &input) {
		return
	}
	tx, err := a.db.BeginTx(r.Context(), nil)
	if err != nil {
		writeError(w, 500, "move_failed", "Could not move section.")
		return
	}
	defer tx.Rollback()
	if input.ParentID != nil {
		var valid int
		err = tx.QueryRowContext(r.Context(), `WITH RECURSIVE descendants(id) AS (SELECT id FROM sections WHERE id=? UNION SELECT s.id FROM sections s JOIN descendants d ON s.parent_id=d.id) SELECT COUNT(*) FROM sections WHERE id=? AND game_id=? AND archived=0 AND id NOT IN (SELECT id FROM descendants)`, item.ID, *input.ParentID, item.GameID).Scan(&valid)
		if err != nil || valid != 1 {
			writeError(w, 400, "invalid_parent", "Choose an active parent in this game outside this section's descendants.")
			return
		}
	}
	rows, err := tx.QueryContext(r.Context(), `SELECT id FROM sections WHERE game_id=? AND parent_id IS ? AND archived=0 AND id<>? ORDER BY sort_order,name,id`, item.GameID, nullableString(input.ParentID), item.ID)
	if err != nil {
		writeError(w, 500, "move_failed", "Could not load section order.")
		return
	}
	ids := []string{}
	for rows.Next() {
		var id string
		if err = rows.Scan(&id); err != nil {
			break
		}
		ids = append(ids, id)
	}
	if err == nil {
		err = rows.Err()
	}
	rows.Close()
	if err != nil {
		writeError(w, 500, "move_failed", "Could not load section order.")
		return
	}
	position := len(ids)
	if input.BeforeID != "" {
		position = -1
		for i, id := range ids {
			if id == input.BeforeID {
				position = i
				break
			}
		}
		if position < 0 {
			writeError(w, 400, "invalid_position", "Choose a sibling in the destination section.")
			return
		}
	}
	ids = append(ids, "")
	copy(ids[position+1:], ids[position:])
	ids[position] = item.ID
	now := time.Now().UTC().Format(time.RFC3339)
	for i, id := range ids {
		if _, err = tx.ExecContext(r.Context(), `UPDATE sections SET parent_id=?,sort_order=?,updated_at=? WHERE id=? AND game_id=? AND archived=0`, nullableString(input.ParentID), i, now, id, item.GameID); err != nil {
			writeError(w, 500, "move_failed", "Could not save section order.")
			return
		}
	}
	if err = tx.Commit(); err != nil {
		writeError(w, 500, "move_failed", "Could not move section.")
		return
	}
	w.WriteHeader(http.StatusNoContent)
}
