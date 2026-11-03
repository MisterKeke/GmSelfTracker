package httpapi

import (
	"bytes"
	"fmt"
	_ "golang.org/x/image/bmp"
	_ "golang.org/x/image/webp"
	"image"
	_ "image/gif"
	_ "image/jpeg"
	"image/png"
	"io"
	"net/http"
	"os"
	"path/filepath"
	"strings"
	"time"
)

const maxUploadSize = 5 << 20

func (a *API) listAssets(w http.ResponseWriter, r *http.Request) {
	rows, err := a.db.QueryContext(r.Context(), `SELECT id,size_bytes FROM uploaded_assets WHERE user_id=? ORDER BY created_at DESC,id`, a.currentUser(r).ID)
	if err != nil {
		writeError(w, 500, "database_error", "Could not load images.")
		return
	}
	defer rows.Close()
	items := []map[string]any{}
	for rows.Next() {
		var id string
		var size int64
		if err := rows.Scan(&id, &size); err != nil {
			writeError(w, 500, "database_error", "Could not load images.")
			return
		}
		items = append(items, map[string]any{"id": id, "url": "/api/assets/" + id, "size_bytes": size})
	}
	if err := rows.Err(); err != nil {
		writeError(w, 500, "database_error", "Could not load images.")
		return
	}
	writeJSON(w, http.StatusOK, map[string]any{"items": items})
}

func (a *API) deleteAsset(w http.ResponseWriter, r *http.Request) {
	tx, err := a.db.BeginTx(r.Context(), nil)
	if err != nil {
		writeError(w, 500, "delete_failed", "Could not delete image.")
		return
	}
	defer tx.Rollback()
	var filename string
	id := r.PathValue("assetID")
	if err := tx.QueryRowContext(r.Context(), `SELECT filename FROM uploaded_assets WHERE id=? AND user_id=?`, id, a.currentUser(r).ID).Scan(&filename); err != nil {
		writeError(w, 404, "not_found", "Image not found.")
		return
	}
	url := "/api/assets/" + id
	var used int
	// Include archived data: restoring a record must not lose its image.
	err = tx.QueryRowContext(r.Context(), `SELECT
	 (SELECT COUNT(*) FROM games WHERE icon=? OR cover_image=?) +
	 (SELECT COUNT(*) FROM sections WHERE icon=?) +
	 (SELECT COUNT(*) FROM object_types WHERE icon=? OR image=?) +
	 (SELECT COUNT(*) FROM field_definitions WHERE icon=?) +
	 (SELECT COUNT(*) FROM objects WHERE image=?)`, url, url, url, url, url, url, url).Scan(&used)
	if err != nil {
		writeError(w, 500, "delete_failed", "Could not check image usage.")
		return
	}
	if used > 0 {
		writeError(w, 409, "image_in_use", "Replace this image on existing or archived records before deleting it.")
		return
	}
	if err := os.Remove(filepath.Join(a.uploadDir, filepath.Base(filename))); err != nil && !os.IsNotExist(err) {
		writeError(w, 500, "delete_failed", "Could not delete image file.")
		return
	}
	if _, err := tx.ExecContext(r.Context(), `DELETE FROM uploaded_assets WHERE id=?`, id); err != nil {
		writeError(w, 500, "delete_failed", "Could not delete image.")
		return
	}
	if err := tx.Commit(); err != nil {
		writeError(w, 500, "delete_failed", "Could not delete image.")
		return
	}
	w.WriteHeader(http.StatusNoContent)
}

func (a *API) uploadAsset(w http.ResponseWriter, r *http.Request) {
	select {
	case a.uploadSlots <- struct{}{}:
		defer func() { <-a.uploadSlots }()
	default:
		w.Header().Set("Retry-After", "5")
		writeError(w, 429, "server_busy", "Please retry the upload in a moment.")
		return
	}
	defer func() {
		if r.MultipartForm != nil {
			_ = r.MultipartForm.RemoveAll()
		}
	}()

	r.Body = http.MaxBytesReader(w, r.Body, maxUploadSize+(1<<20))
	if err := r.ParseMultipartForm(maxUploadSize); err != nil {
		writeError(w, http.StatusBadRequest, "upload_too_large", "Choose an image smaller than 5 MB.")
		return
	}
	file, _, err := r.FormFile("file")
	if err != nil {
		writeError(w, http.StatusBadRequest, "missing_file", "Choose an image to upload.")
		return
	}
	defer file.Close()
	data, err := io.ReadAll(io.LimitReader(file, maxUploadSize+1))
	if err != nil || len(data) == 0 || len(data) > maxUploadSize {
		writeError(w, http.StatusBadRequest, "upload_too_large", "Choose an image between 1 byte and 5 MB.")
		return
	}
	config, format, decodeErr := image.DecodeConfig(bytes.NewReader(data))
	if decodeErr != nil {
		writeError(w, http.StatusUnsupportedMediaType, "invalid_image", "The uploaded file is not a valid PNG, JPEG, GIF, WebP, or BMP image.")
		return
	}
	if format != "png" && format != "jpeg" && format != "gif" && format != "webp" && format != "bmp" {
		writeError(w, http.StatusUnsupportedMediaType, "invalid_image", "Upload a PNG, JPEG, GIF, WebP, or BMP image.")
		return
	}
	mimeType, extension := "image/png", ".png"
	if config.Width <= 0 || config.Height <= 0 || config.Width > 6000 || config.Height > 6000 || int64(config.Width)*int64(config.Height) > 16000000 {
		writeError(w, http.StatusBadRequest, "invalid_image_dimensions", "Images must fit within 6000 × 6000 and 16 million pixels.")
		return
	}
	// Decode pixel data and normalize it, removing appended payloads and metadata.
	// Animated formats use their first frame; bound decoded memory before allocation.
	decoded, _, err := image.Decode(bytes.NewReader(data))
	if err != nil {
		writeError(w, 415, "invalid_image", "The image is damaged or incomplete.")
		return
	}
	var normalized bytes.Buffer
	if err := png.Encode(&normalized, decoded); err != nil {
		writeError(w, 400, "invalid_image", "Could not process image.")
		return
	}
	if normalized.Len() > maxUploadSize {
		writeError(w, 400, "upload_too_large", "The processed image exceeds 5 MB. Choose a smaller image.")
		return
	}
	data = normalized.Bytes()
	var gameID any
	if value := strings.TrimSpace(r.FormValue("game_id")); value != "" {
		if _, err := a.loadGame(r, value, false); err != nil {
			writeError(w, http.StatusBadRequest, "invalid_game", "Choose a game you own.")
			return
		}
		gameID = value
	}
	id, err := randomID()
	if err != nil {
		writeError(w, http.StatusInternalServerError, "upload_failed", "Could not save image.")
		return
	}
	filename := id + extension
	path := filepath.Join(a.uploadDir, filename)
	if err := os.MkdirAll(a.uploadDir, 0o700); err != nil {
		writeError(w, http.StatusInternalServerError, "upload_failed", "Could not save image.")
		return
	}
	if err := os.WriteFile(path, data, 0o600); err != nil {
		writeError(w, http.StatusInternalServerError, "upload_failed", "Could not save image.")
		return
	}
	user := a.currentUser(r)
	result, err := a.db.ExecContext(r.Context(), `INSERT INTO uploaded_assets(id,user_id,game_id,filename,mime_type,size_bytes,created_at) SELECT ?,?,?,?,?,?,? WHERE (SELECT COALESCE(SUM(size_bytes),0) FROM uploaded_assets WHERE user_id=?) + ? <= ? AND (SELECT COUNT(*) FROM uploaded_assets WHERE user_id=?) < 1000`, id, user.ID, gameID, filename, mimeType, len(data), time.Now().UTC().Format(time.RFC3339), user.ID, len(data), 250<<20, user.ID)
	if err != nil {
		_ = os.Remove(path)
		writeError(w, http.StatusInternalServerError, "upload_failed", "Could not save image.")
		return
	}
	if count, _ := result.RowsAffected(); count != 1 {
		_ = os.Remove(path)
		writeError(w, 409, "upload_quota", "The account upload limit is 250 MB or 1000 images.")
		return
	}
	writeJSON(w, http.StatusCreated, map[string]any{"id": id, "url": "/api/assets/" + id, "mime_type": mimeType, "size_bytes": len(data)})
}

func (a *API) getAsset(w http.ResponseWriter, r *http.Request) {
	var filename, mimeType string
	var owner string
	err := a.db.QueryRowContext(r.Context(), `SELECT filename,mime_type,user_id FROM uploaded_assets WHERE id=?`, r.PathValue("assetID")).Scan(&filename, &mimeType, &owner)
	if err != nil || owner != a.currentUser(r).ID {
		writeError(w, http.StatusNotFound, "not_found", "Image not found.")
		return
	}
	path := filepath.Join(a.uploadDir, filepath.Base(filename))
	file, err := os.Open(path)
	if err != nil {
		writeError(w, http.StatusNotFound, "not_found", "Image not found.")
		return
	}
	defer file.Close()
	w.Header().Set("Content-Type", mimeType)
	w.Header().Set("Cache-Control", "private, no-store")
	w.Header().Set("X-Content-Type-Options", "nosniff")
	if _, err := io.Copy(w, file); err != nil {
		a.logger.Debug("asset response ended", "error", fmt.Errorf("serve image: %w", err))
	}
}
