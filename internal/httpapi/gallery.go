package httpapi

import (
	"net/http"
	"strings"
	"time"
)

type objectGalleryImage struct {
	ID        string `json:"id"`
	ObjectID  string `json:"object_id"`
	AssetID   string `json:"asset_id"`
	URL       string `json:"url"`
	Caption   string `json:"caption"`
	SortOrder int    `json:"sort_order"`
	IsPrimary bool   `json:"is_primary"`
	CreatedAt string `json:"created_at"`
}

func (a *API) listObjectGallery(w http.ResponseWriter, r *http.Request) {
	object, err := a.loadObjectAny(r, r.PathValue("objectID"))
	if err != nil {
		notFoundOrError(w, err, "Object")
		return
	}
	var typeArchived int
	if err := a.db.QueryRowContext(r.Context(), `SELECT archived FROM object_types WHERE id=?`, object.ObjectTypeID).Scan(&typeArchived); err != nil {
		writeError(w, http.StatusInternalServerError, "database_error", "Could not load object type status.")
		return
	}
	rows, err := a.db.QueryContext(r.Context(), `SELECT g.id,g.object_id,g.asset_id,g.caption,g.sort_order,g.is_primary,g.created_at
		FROM object_gallery g WHERE g.object_id=? ORDER BY g.sort_order,g.id`, object.ID)
	if err != nil {
		writeError(w, http.StatusInternalServerError, "database_error", "Could not load the object gallery.")
		return
	}
	defer rows.Close()
	items := make([]objectGalleryImage, 0)
	for rows.Next() {
		var item objectGalleryImage
		var primary int
		if err := rows.Scan(&item.ID, &item.ObjectID, &item.AssetID, &item.Caption, &item.SortOrder, &primary, &item.CreatedAt); err != nil {
			writeError(w, http.StatusInternalServerError, "database_error", "Could not load the object gallery.")
			return
		}
		item.URL = "/api/assets/" + item.AssetID
		item.IsPrimary = primary != 0
		items = append(items, item)
	}
	if err := rows.Err(); err != nil {
		writeError(w, http.StatusInternalServerError, "database_error", "Could not load the object gallery.")
		return
	}
	writeJSON(w, http.StatusOK, map[string]any{"items": items, "object_id": object.ID, "archived": object.Archived || typeArchived != 0})
}

func (a *API) addObjectGalleryImage(w http.ResponseWriter, r *http.Request) {
	object, err := a.loadObject(r, r.PathValue("objectID"))
	if err != nil {
		notFoundOrError(w, err, "Object")
		return
	}
	var input struct {
		AssetID string `json:"asset_id"`
		Caption string `json:"caption"`
	}
	if !decodeJSON(w, r, &input) {
		return
	}
	input.AssetID = strings.TrimSpace(input.AssetID)
	input.Caption = strings.TrimSpace(input.Caption)
	if input.AssetID == "" || len(input.Caption) > 500 {
		writeError(w, http.StatusBadRequest, "invalid_gallery_image", "Choose an uploaded image and use a caption of at most 500 characters.")
		return
	}
	var available int
	if err := a.db.QueryRowContext(r.Context(), `SELECT COUNT(*) FROM uploaded_assets WHERE id=? AND user_id=? AND deleted_at IS NULL`, input.AssetID, a.currentUser(r).ID).Scan(&available); err != nil {
		writeError(w, http.StatusInternalServerError, "database_error", "Could not validate the uploaded image.")
		return
	}
	if available != 1 {
		writeError(w, http.StatusBadRequest, "invalid_gallery_image", "The image is unavailable or belongs to another account.")
		return
	}
	var count int
	if err := a.db.QueryRowContext(r.Context(), `SELECT COUNT(*) FROM object_gallery WHERE object_id=?`, object.ID).Scan(&count); err != nil {
		writeError(w, http.StatusInternalServerError, "database_error", "Could not check the object gallery.")
		return
	}
	if count >= 50 {
		writeError(w, http.StatusConflict, "gallery_limit", "An object can contain at most 50 gallery images.")
		return
	}
	id, err := randomID()
	if err != nil {
		writeError(w, http.StatusInternalServerError, "gallery_failed", "Could not add the image.")
		return
	}
	var nextOrder, currentPrimary int
	if err := a.db.QueryRowContext(r.Context(), `SELECT COALESCE(MAX(sort_order)+1,0),COALESCE(MAX(is_primary),0) FROM object_gallery WHERE object_id=?`, object.ID).Scan(&nextOrder, &currentPrimary); err != nil {
		writeError(w, http.StatusInternalServerError, "database_error", "Could not prepare the gallery image.")
		return
	}
	primary := count == 0 || currentPrimary == 0
	result, err := a.db.ExecContext(r.Context(), `INSERT INTO object_gallery(id,object_id,asset_id,caption,sort_order,is_primary,created_at) VALUES(?,?,?,?,?,?,?)`, id, object.ID, input.AssetID, input.Caption, nextOrder, boolInt(primary), time.Now().UTC().Format(time.RFC3339Nano))
	if err != nil {
		writeError(w, http.StatusInternalServerError, "gallery_failed", "Could not add the image to this gallery.")
		return
	}
	_ = result
	writeJSON(w, http.StatusCreated, map[string]any{"id": id, "object_id": object.ID, "asset_id": input.AssetID, "url": "/api/assets/" + input.AssetID, "caption": input.Caption, "sort_order": nextOrder, "is_primary": primary})
}

func (a *API) updateObjectGalleryImage(w http.ResponseWriter, r *http.Request) {
	id := r.PathValue("galleryID")
	object, err := a.loadGalleryImageObject(r, id)
	if err != nil {
		notFoundOrError(w, err, "Gallery image")
		return
	}
	if object.Archived {
		writeError(w, http.StatusConflict, "object_archived", "Restore the object before editing its gallery.")
		return
	}
	var input struct {
		Caption   *string `json:"caption"`
		SortOrder *int    `json:"sort_order"`
		IsPrimary *bool   `json:"is_primary"`
	}
	if !decodeJSON(w, r, &input) {
		return
	}
	var caption string
	var sortOrder int
	var primary int
	var assetID, createdAt string
	if err := a.db.QueryRowContext(r.Context(), `SELECT asset_id,caption,sort_order,is_primary,created_at FROM object_gallery WHERE id=? AND object_id=?`, id, object.ID).Scan(&assetID, &caption, &sortOrder, &primary, &createdAt); err != nil {
		notFoundOrError(w, err, "Gallery image")
		return
	}
	if input.Caption != nil {
		caption = strings.TrimSpace(*input.Caption)
		if len(caption) > 500 {
			writeError(w, http.StatusBadRequest, "invalid_caption", "Gallery captions may contain up to 500 characters.")
			return
		}
	}
	if input.SortOrder != nil {
		if *input.SortOrder < 0 || *input.SortOrder > 10000 {
			writeError(w, http.StatusBadRequest, "invalid_sort_order", "Gallery order must be between 0 and 10,000.")
			return
		}
		sortOrder = *input.SortOrder
	}
	if input.IsPrimary != nil {
		if *input.IsPrimary {
			primary = 1
		} else {
			primary = 0
		}
	}
	tx, err := a.db.BeginTx(r.Context(), nil)
	if err != nil {
		writeError(w, http.StatusInternalServerError, "gallery_failed", "Could not update the gallery image.")
		return
	}
	defer tx.Rollback()
	if primary == 1 {
		if _, err := tx.ExecContext(r.Context(), `UPDATE object_gallery SET is_primary=0 WHERE object_id=?`, object.ID); err != nil {
			writeError(w, http.StatusInternalServerError, "gallery_failed", "Could not choose the primary image.")
			return
		}
	}
	if _, err := tx.ExecContext(r.Context(), `UPDATE object_gallery SET caption=?,sort_order=?,is_primary=? WHERE id=? AND object_id=?`, caption, sortOrder, primary, id, object.ID); err != nil {
		writeError(w, http.StatusInternalServerError, "gallery_failed", "Could not update the gallery image.")
		return
	}
	if err := tx.Commit(); err != nil {
		writeError(w, http.StatusInternalServerError, "gallery_failed", "Could not update the gallery image.")
		return
	}
	writeJSON(w, http.StatusOK, map[string]any{"id": id, "object_id": object.ID, "asset_id": assetID, "url": "/api/assets/" + assetID, "caption": caption, "sort_order": sortOrder, "is_primary": primary == 1, "created_at": createdAt})
}

func (a *API) deleteObjectGalleryImage(w http.ResponseWriter, r *http.Request) {
	object, err := a.loadGalleryImageObject(r, r.PathValue("galleryID"))
	if err != nil {
		notFoundOrError(w, err, "Gallery image")
		return
	}
	if object.Archived {
		writeError(w, http.StatusConflict, "object_archived", "Restore the object before editing its gallery.")
		return
	}
	if _, err := a.db.ExecContext(r.Context(), `DELETE FROM object_gallery WHERE id=? AND object_id=?`, r.PathValue("galleryID"), object.ID); err != nil {
		writeError(w, http.StatusInternalServerError, "gallery_failed", "Could not remove the image from this gallery.")
		return
	}
	w.WriteHeader(http.StatusNoContent)
}

func (a *API) loadGalleryImageObject(r *http.Request, galleryID string) (objectRecord, error) {
	var item objectRecord
	var archived int
	err := a.db.QueryRowContext(r.Context(), `SELECT o.id,o.name,o.object_type_id,t.name,o.description,o.image,o.updated_at,o.created_at,o.game_id,CASE WHEN o.archived=1 OR t.archived=1 THEN 1 ELSE 0 END
		FROM object_gallery p JOIN objects o ON o.id=p.object_id JOIN object_types t ON t.id=o.object_type_id JOIN games g ON g.id=o.game_id
		WHERE p.id=? AND g.user_id=? AND g.archived=0`, galleryID, a.currentUser(r).ID).Scan(&item.ID, &item.Name, &item.ObjectTypeID, &item.TypeName, &item.Description, &item.Image, &item.UpdatedAt, &item.CreatedAt, &item.GameID, &archived)
	item.Archived = archived != 0
	return item, err
}
