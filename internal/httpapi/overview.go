package httpapi

import (
	"net/http"
)

type overviewResponse struct {
	Game         game           `json:"game"`
	ObjectCount  int            `json:"object_count"`
	SectionCount int            `json:"section_count"`
	Recent       []objectRecord `json:"recent_objects"`
}

func (a *API) getOverview(w http.ResponseWriter, r *http.Request) {
	item, err := a.loadGame(r, r.PathValue("gameID"), false)
	if err != nil {
		notFoundOrError(w, err, "Game")
		return
	}
	if err := a.db.QueryRowContext(r.Context(), `SELECT COUNT(*) FROM objects o JOIN object_types t ON t.id=o.object_type_id WHERE o.game_id=? AND o.archived=0 AND t.archived=0`, item.ID).Scan(&item.ObjectCount); err != nil {
		writeError(w, http.StatusInternalServerError, "database_error", "Could not count game objects.")
		return
	}
	if err := a.db.QueryRowContext(r.Context(), `SELECT COUNT(*) FROM sections WHERE game_id=? AND archived=0`, item.ID).Scan(&item.SectionCount); err != nil {
		writeError(w, http.StatusInternalServerError, "database_error", "Could not count game sections.")
		return
	}
	rows, err := a.db.QueryContext(r.Context(), `SELECT o.id,o.name,o.object_type_id,t.name,o.description,o.image,o.updated_at,o.created_at,o.archived FROM objects o JOIN object_types t ON t.id=o.object_type_id WHERE o.game_id=? AND o.archived=0 AND t.archived=0 ORDER BY o.updated_at DESC,o.id ASC LIMIT 6`, item.ID)
	if err != nil {
		writeError(w, http.StatusInternalServerError, "database_error", "Could not load recent game objects.")
		return
	}
	recent := make([]objectRecord, 0, 6)
	for rows.Next() {
		object, scanErr := scanObjectRecord(rows, item.ID)
		if scanErr != nil {
			rows.Close()
			writeError(w, http.StatusInternalServerError, "database_error", "Could not load recent game objects.")
			return
		}
		recent = append(recent, object)
	}
	if err := rows.Err(); err != nil {
		rows.Close()
		writeError(w, http.StatusInternalServerError, "database_error", "Could not load recent game objects.")
		return
	}
	if err := rows.Close(); err != nil {
		writeError(w, http.StatusInternalServerError, "database_error", "Could not load recent game objects.")
		return
	}
	if err := a.hydrateObjects(r.Context(), recent); err != nil {
		writeError(w, http.StatusInternalServerError, "database_error", "Could not load recent game object data.")
		return
	}
	writeJSON(w, http.StatusOK, overviewResponse{Game: item, ObjectCount: item.ObjectCount, SectionCount: item.SectionCount, Recent: recent})
}
