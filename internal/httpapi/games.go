package httpapi

import (
	"database/sql"
	"net/http"
	"strconv"
	"strings"
	"time"
)

type game struct {
	ID            string `json:"id"`
	Name          string `json:"name"`
	Description   string `json:"description"`
	CoverImage    string `json:"cover_image"`
	PersonalNotes string `json:"personal_notes"`
	Archived      bool   `json:"archived"`
	CreatedAt     string `json:"created_at"`
	UpdatedAt     string `json:"updated_at"`
	ObjectCount   int    `json:"object_count,omitempty"`
	SectionCount  int    `json:"section_count,omitempty"`
}

func (a *API) listGames(w http.ResponseWriter, r *http.Request) {
	user := a.currentUser(r)
	includeArchived := r.URL.Query().Get("archived") == "true"
	limit := queryInt(r, "limit", 100, 1, 200)
	offset := queryInt(r, "offset", 0, 0, 1000000)
	query := `SELECT g.id,g.name,g.description,g.cover_image,g.personal_notes,g.archived,g.created_at,g.updated_at,
		(SELECT COUNT(*) FROM objects o WHERE o.game_id=g.id AND o.archived=0),
		(SELECT COUNT(*) FROM sections s WHERE s.game_id=g.id AND s.archived=0)
		FROM games g WHERE g.user_id=?`
	if !includeArchived {
		query += ` AND g.archived=0`
	}
	query += ` ORDER BY g.updated_at DESC LIMIT ? OFFSET ?`
	rows, err := a.db.QueryContext(r.Context(), query, user.ID, limit, offset)
	if err != nil {
		writeError(w, http.StatusInternalServerError, "database_error", "Could not load your games.")
		return
	}
	defer rows.Close()
	items := make([]game, 0)
	for rows.Next() {
		var item game
		var archived int
		if err := rows.Scan(&item.ID, &item.Name, &item.Description, &item.CoverImage, &item.PersonalNotes, &archived, &item.CreatedAt, &item.UpdatedAt, &item.ObjectCount, &item.SectionCount); err != nil {
			writeError(w, http.StatusInternalServerError, "database_error", "Could not load your games.")
			return
		}
		item.Archived = archived != 0
		items = append(items, item)
	}
	writeJSON(w, http.StatusOK, map[string]any{"items": items, "limit": limit, "offset": offset})
}

func (a *API) createGame(w http.ResponseWriter, r *http.Request) {
	var input struct {
		Name          string `json:"name"`
		Description   string `json:"description"`
		CoverImage    string `json:"cover_image"`
		PersonalNotes string `json:"personal_notes"`
	}
	if !decodeJSON(w, r, &input) {
		return
	}
	input.Name = strings.TrimSpace(input.Name)
	if input.Name == "" || len(input.Name) > 120 {
		writeError(w, http.StatusBadRequest, "invalid_name", "Game name must be between 1 and 120 characters.")
		return
	}
	id, err := randomID()
	if err != nil {
		writeError(w, http.StatusInternalServerError, "create_failed", "Could not create the game.")
		return
	}
	now := time.Now().UTC().Format(time.RFC3339)
	user := a.currentUser(r)
	_, err = a.db.ExecContext(r.Context(), `INSERT INTO games(id,user_id,name,description,cover_image,personal_notes,created_at,updated_at) VALUES(?,?,?,?,?,?,?,?)`, id, user.ID, input.Name, input.Description, input.CoverImage, input.PersonalNotes, now, now)
	if err != nil {
		writeError(w, http.StatusInternalServerError, "create_failed", "Could not create the game.")
		return
	}
	writeJSON(w, http.StatusCreated, game{ID: id, Name: input.Name, Description: input.Description, CoverImage: input.CoverImage, PersonalNotes: input.PersonalNotes, CreatedAt: now, UpdatedAt: now})
}

func (a *API) getGame(w http.ResponseWriter, r *http.Request) {
	item, err := a.loadGame(r, r.PathValue("gameID"), false)
	if err != nil {
		notFoundOrError(w, err, "Game")
		return
	}
	writeJSON(w, http.StatusOK, item)
}

func (a *API) updateGame(w http.ResponseWriter, r *http.Request) {
	var input struct {
		Name          *string `json:"name"`
		Description   *string `json:"description"`
		CoverImage    *string `json:"cover_image"`
		PersonalNotes *string `json:"personal_notes"`
	}
	if !decodeJSON(w, r, &input) {
		return
	}
	item, err := a.loadGame(r, r.PathValue("gameID"), true)
	if err != nil {
		notFoundOrError(w, err, "Game")
		return
	}
	if input.Name != nil {
		name := strings.TrimSpace(*input.Name)
		if name == "" || len(name) > 120 {
			writeError(w, http.StatusBadRequest, "invalid_name", "Game name must be between 1 and 120 characters.")
			return
		}
		item.Name = name
	}
	if input.Description != nil {
		item.Description = *input.Description
	}
	if input.CoverImage != nil {
		item.CoverImage = *input.CoverImage
	}
	if input.PersonalNotes != nil {
		item.PersonalNotes = *input.PersonalNotes
	}
	item.UpdatedAt = time.Now().UTC().Format(time.RFC3339)
	_, err = a.db.ExecContext(r.Context(), `UPDATE games SET name=?,description=?,cover_image=?,personal_notes=?,updated_at=? WHERE id=? AND user_id=?`, item.Name, item.Description, item.CoverImage, item.PersonalNotes, item.UpdatedAt, item.ID, a.currentUser(r).ID)
	if err != nil {
		writeError(w, http.StatusInternalServerError, "update_failed", "Could not save game changes.")
		return
	}
	writeJSON(w, http.StatusOK, item)
}

func (a *API) archiveGame(w http.ResponseWriter, r *http.Request) { a.setGameArchived(w, r, true) }
func (a *API) restoreGame(w http.ResponseWriter, r *http.Request) { a.setGameArchived(w, r, false) }

func (a *API) setGameArchived(w http.ResponseWriter, r *http.Request, archived bool) {
	user := a.currentUser(r)
	result, err := a.db.ExecContext(r.Context(), `UPDATE games SET archived=?,updated_at=? WHERE id=? AND user_id=?`, boolInt(archived), time.Now().UTC().Format(time.RFC3339), r.PathValue("gameID"), user.ID)
	if err != nil {
		writeError(w, http.StatusInternalServerError, "update_failed", "Could not update the game.")
		return
	}
	if n, _ := result.RowsAffected(); n == 0 {
		writeError(w, http.StatusNotFound, "not_found", "Game not found.")
		return
	}
	w.WriteHeader(http.StatusNoContent)
}

func (a *API) deleteGame(w http.ResponseWriter, r *http.Request) {
	user := a.currentUser(r)
	result, err := a.db.ExecContext(r.Context(), `DELETE FROM games WHERE id=? AND user_id=?`, r.PathValue("gameID"), user.ID)
	if err != nil {
		writeError(w, http.StatusInternalServerError, "delete_failed", "Could not delete the game.")
		return
	}
	if n, _ := result.RowsAffected(); n == 0 {
		writeError(w, http.StatusNotFound, "not_found", "Game not found.")
		return
	}
	w.WriteHeader(http.StatusNoContent)
}

func (a *API) loadGame(r *http.Request, id string, includeArchived bool) (game, error) {
	user := a.currentUser(r)
	var item game
	var archived int
	query := `SELECT id,name,description,cover_image,personal_notes,archived,created_at,updated_at FROM games WHERE id=? AND user_id=?`
	if !includeArchived {
		query += ` AND archived=0`
	}
	err := a.db.QueryRowContext(r.Context(), query, id, user.ID).Scan(&item.ID, &item.Name, &item.Description, &item.CoverImage, &item.PersonalNotes, &archived, &item.CreatedAt, &item.UpdatedAt)
	item.Archived = archived != 0
	return item, err
}

func queryInt(r *http.Request, name string, fallback, min, max int) int {
	value, err := strconv.Atoi(r.URL.Query().Get(name))
	if err != nil {
		return fallback
	}
	if value < min {
		return min
	}
	if value > max {
		return max
	}
	return value
}

func boolInt(value bool) int {
	if value {
		return 1
	}
	return 0
}

func notFoundOrError(w http.ResponseWriter, err error, entity string) {
	if err == sql.ErrNoRows {
		writeError(w, http.StatusNotFound, "not_found", entity+" not found.")
		return
	}
	writeError(w, http.StatusInternalServerError, "database_error", "Could not load the requested record.")
}
