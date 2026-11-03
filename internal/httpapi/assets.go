package httpapi

import (
	"bytes"
	"fmt"
	"image"
	_ "image/gif"
	_ "image/jpeg"
	_ "image/png"
	"io"
	"net/http"
	"os"
	"path/filepath"
	"strings"
	"time"
)

const maxUploadSize = 5 << 20

func (a *API) uploadAsset(w http.ResponseWriter, r *http.Request) {
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
		writeError(w, http.StatusUnsupportedMediaType, "invalid_image", "The uploaded file is not a valid PNG, JPEG, or GIF image.")
		return
	}
	mimeType := ""
	extension := ""
	switch format {
	case "png":
		extension = ".png"
		mimeType = "image/png"
	case "jpeg":
		extension = ".jpg"
		mimeType = "image/jpeg"
	case "gif":
		extension = ".gif"
		mimeType = "image/gif"
	default:
		writeError(w, http.StatusUnsupportedMediaType, "invalid_image", "Upload a PNG, JPEG, or GIF image.")
		return
	}
	if config.Width <= 0 || config.Height <= 0 || config.Width > 6000 || config.Height > 6000 || int64(config.Width)*int64(config.Height) > 40000000 {
		writeError(w, http.StatusBadRequest, "invalid_image_dimensions", "Image dimensions must be no larger than 6000 × 6000 pixels.")
		return
	}
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
	if err := os.MkdirAll(a.uploadDir, 0o755); err != nil {
		writeError(w, http.StatusInternalServerError, "upload_failed", "Could not save image.")
		return
	}
	if err := os.WriteFile(path, data, 0o600); err != nil {
		writeError(w, http.StatusInternalServerError, "upload_failed", "Could not save image.")
		return
	}
	user := a.currentUser(r)
	_, err = a.db.ExecContext(r.Context(), `INSERT INTO uploaded_assets(id,user_id,game_id,filename,mime_type,size_bytes,created_at) VALUES(?,?,?,?,?,?,?)`, id, user.ID, gameID, filename, mimeType, len(data), time.Now().UTC().Format(time.RFC3339))
	if err != nil {
		_ = os.Remove(path)
		writeError(w, http.StatusInternalServerError, "upload_failed", "Could not save image.")
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
	w.Header().Set("Cache-Control", "private, max-age=3600")
	w.Header().Set("X-Content-Type-Options", "nosniff")
	if _, err := io.Copy(w, file); err != nil {
		a.logger.Debug("asset response ended", "error", fmt.Errorf("serve image: %w", err))
	}
}
