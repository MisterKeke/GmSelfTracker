package httpapi

import (
	"context"
	"crypto/rand"
	"database/sql"
	"encoding/base64"
	"encoding/hex"
	"encoding/json"
	"errors"
	"io"
	"log/slog"
	"net/http"
	"strings"
	"time"

	"gametracker/web"
)

type API struct {
	db        *sql.DB
	uploadDir string
	secure    bool
	logger    *slog.Logger
	limiter   *loginLimiter
}

type userSession struct {
	ID       string `json:"id"`
	Username string `json:"username"`
	CSRF     string `json:"csrf_token,omitempty"`
}

type contextKey string

const sessionContextKey contextKey = "user-session"

func New(db *sql.DB, uploadDir string, production bool, logger *slog.Logger) http.Handler {
	if logger == nil {
		logger = slog.Default()
	}
	a := &API{db: db, uploadDir: uploadDir, secure: production, logger: logger, limiter: newLoginLimiter()}
	mux := http.NewServeMux()
	mux.HandleFunc("GET /api/health", a.health)
	mux.HandleFunc("POST /api/auth/register", a.register)
	mux.HandleFunc("POST /api/auth/login", a.login)
	mux.Handle("POST /api/auth/password", a.protected(http.HandlerFunc(a.changePassword)))
	mux.Handle("POST /api/auth/logout", a.protected(http.HandlerFunc(a.logout)))
	mux.Handle("GET /api/auth/me", a.protected(http.HandlerFunc(a.me)))

	protected := func(pattern string, h http.HandlerFunc) { mux.Handle(pattern, a.protected(h)) }
	protected("GET /api/games", a.listGames)
	protected("POST /api/games", a.createGame)
	protected("GET /api/games/{gameID}", a.getGame)
	protected("PATCH /api/games/{gameID}", a.updateGame)
	protected("DELETE /api/games/{gameID}", a.deleteGame)
	protected("POST /api/games/{gameID}/archive", a.archiveGame)
	protected("POST /api/games/{gameID}/restore", a.restoreGame)
	protected("GET /api/games/{gameID}/sections", a.listSections)
	protected("POST /api/games/{gameID}/sections", a.createSection)
	protected("GET /api/games/{gameID}/object-types", a.listObjectTypes)
	protected("POST /api/games/{gameID}/object-types", a.createObjectType)
	protected("GET /api/games/{gameID}/objects", a.listObjects)
	protected("POST /api/games/{gameID}/objects", a.createObject)
	protected("GET /api/games/{gameID}/goals", a.listGoals)
	protected("POST /api/games/{gameID}/goals", a.createGoal)

	protected("PATCH /api/sections/{sectionID}", a.updateSection)
	protected("DELETE /api/sections/{sectionID}", a.archiveSection)
	protected("POST /api/sections/{sectionID}/restore", a.restoreSection)
	protected("GET /api/sections/{sectionID}/placements", a.listPlacements)
	protected("POST /api/sections/{sectionID}/placements", a.addPlacement)
	protected("DELETE /api/sections/{sectionID}/placements/{objectID}", a.removePlacement)

	protected("GET /api/object-types/{typeID}", a.getObjectType)
	protected("PATCH /api/object-types/{typeID}", a.updateObjectType)
	protected("POST /api/object-types/{typeID}/fields", a.createField)
	protected("PATCH /api/fields/{fieldID}", a.updateField)
	protected("POST /api/fields/{fieldID}/archive", a.archiveField)
	protected("POST /api/fields/{fieldID}/restore", a.restoreField)

	protected("GET /api/objects/{objectID}", a.getObject)
	protected("PATCH /api/objects/{objectID}", a.updateObject)
	protected("PATCH /api/objects/{objectID}/values", a.updateObjectValues)
	protected("DELETE /api/objects/{objectID}", a.deleteObject)
	protected("POST /api/objects/{objectID}/archive", a.archiveObject)
	protected("POST /api/objects/{objectID}/restore", a.restoreObject)

	protected("PATCH /api/goals/{goalID}", a.updateGoal)
	protected("DELETE /api/goals/{goalID}", a.deleteGoal)
	protected("PATCH /api/goal-tasks/{taskID}", a.updateGoalTask)
	protected("POST /api/goals/{goalID}/tasks", a.createGoalTask)
	protected("DELETE /api/goal-tasks/{taskID}", a.deleteGoalTask)

	protected("POST /api/assets", a.uploadAsset)
	protected("GET /api/assets/{assetID}", a.getAsset)

	// Static HTML, CSS, and modules are embedded so the server works from any current directory.
	static, _ := web.Files()
	mux.Handle("/", http.FileServer(http.FS(static)))
	return requestLogger(logger, mux)
}

func (a *API) protected(next http.Handler) http.Handler {
	return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		cookie, err := r.Cookie("gametracker_session")
		if err != nil || cookie.Value == "" {
			writeError(w, http.StatusUnauthorized, "authentication_required", "Please sign in to continue.")
			return
		}
		user, err := a.findSession(r.Context(), cookie.Value)
		if err != nil {
			writeError(w, http.StatusUnauthorized, "authentication_required", "Your session has expired. Please sign in again.")
			return
		}
		if r.Method != http.MethodGet && r.Method != http.MethodHead && r.Method != http.MethodOptions {
			provided := r.Header.Get("X-CSRF-Token")
			if provided == "" || !constantTimeEqual(provided, user.CSRF) {
				writeError(w, http.StatusForbidden, "csrf_failed", "Refresh the page and try again.")
				return
			}
		}
		next.ServeHTTP(w, r.WithContext(context.WithValue(r.Context(), sessionContextKey, user)))
	})
}

func (a *API) currentUser(r *http.Request) userSession {
	user, _ := r.Context().Value(sessionContextKey).(userSession)
	return user
}

func (a *API) health(w http.ResponseWriter, r *http.Request) {
	if err := a.db.PingContext(r.Context()); err != nil {
		writeError(w, http.StatusServiceUnavailable, "database_unavailable", "The local database is unavailable.")
		return
	}
	writeJSON(w, http.StatusOK, map[string]string{"status": "ok"})
}

func (a *API) newSession(ctx context.Context, userID string) (userSession, string, error) {
	token, err := randomToken(32)
	if err != nil {
		return userSession{}, "", err
	}
	csrf, err := randomToken(24)
	if err != nil {
		return userSession{}, "", err
	}
	var user userSession
	user.ID = userID
	user.CSRF = csrf
	if err := a.db.QueryRowContext(ctx, `SELECT username FROM users WHERE id = ?`, userID).Scan(&user.Username); err != nil {
		return userSession{}, "", err
	}
	_, err = a.db.ExecContext(ctx, `INSERT INTO sessions(token_hash, csrf_token, user_id, expires_at, created_at) VALUES(?,?,?,?,?)`, tokenHash(token), csrf, userID, time.Now().UTC().Add(30*24*time.Hour).Format(time.RFC3339), time.Now().UTC().Format(time.RFC3339))
	if err != nil {
		return userSession{}, "", err
	}
	return user, token, nil
}

func (a *API) findSession(ctx context.Context, token string) (userSession, error) {
	var user userSession
	var expires string
	err := a.db.QueryRowContext(ctx, `SELECT u.id, u.username, s.csrf_token, s.expires_at FROM sessions s JOIN users u ON u.id = s.user_id WHERE s.token_hash = ?`, tokenHash(token)).Scan(&user.ID, &user.Username, &user.CSRF, &expires)
	if err != nil {
		return userSession{}, err
	}
	if expiry, err := time.Parse(time.RFC3339, expires); err != nil || time.Now().UTC().After(expiry) {
		_, _ = a.db.ExecContext(ctx, `DELETE FROM sessions WHERE token_hash = ?`, tokenHash(token))
		return userSession{}, errors.New("expired session")
	}
	return user, nil
}

func (a *API) setSessionCookie(w http.ResponseWriter, token string, expires time.Time) {
	http.SetCookie(w, &http.Cookie{Name: "gametracker_session", Value: token, Path: "/", HttpOnly: true, Secure: a.secure, SameSite: http.SameSiteLaxMode, Expires: expires, MaxAge: int(time.Until(expires).Seconds())})
}

func (a *API) clearSessionCookie(w http.ResponseWriter) {
	http.SetCookie(w, &http.Cookie{Name: "gametracker_session", Value: "", Path: "/", HttpOnly: true, Secure: a.secure, SameSite: http.SameSiteLaxMode, MaxAge: -1})
}

func writeJSON(w http.ResponseWriter, status int, value any) {
	w.Header().Set("Content-Type", "application/json; charset=utf-8")
	w.Header().Set("X-Content-Type-Options", "nosniff")
	w.WriteHeader(status)
	if err := json.NewEncoder(w).Encode(value); err != nil {
		return
	}
}

type errorBody struct {
	Error struct {
		Code    string `json:"code"`
		Message string `json:"message"`
	} `json:"error"`
}

func writeError(w http.ResponseWriter, status int, code, message string) {
	var body errorBody
	body.Error.Code, body.Error.Message = code, message
	writeJSON(w, status, body)
}

func decodeJSON(w http.ResponseWriter, r *http.Request, target any) bool {
	r.Body = http.MaxBytesReader(w, r.Body, 1<<20)
	decoder := json.NewDecoder(r.Body)
	decoder.DisallowUnknownFields()
	if err := decoder.Decode(target); err != nil {
		writeError(w, http.StatusBadRequest, "invalid_json", "The request body is not valid JSON.")
		return false
	}
	if err := decoder.Decode(&struct{}{}); err != io.EOF {
		writeError(w, http.StatusBadRequest, "invalid_json", "Send one JSON value per request.")
		return false
	}
	return true
}

func requestLogger(logger *slog.Logger, next http.Handler) http.Handler {
	return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		if strings.HasPrefix(r.URL.Path, "/api/") && r.URL.Path != "/api/health" {
			logger.Debug("http request", "method", r.Method, "path", r.URL.Path)
		}
		next.ServeHTTP(w, r)
	})
}

func randomID() (string, error) {
	var raw [16]byte
	if _, err := rand.Read(raw[:]); err != nil {
		return "", err
	}
	raw[6] = (raw[6] & 0x0f) | 0x40
	raw[8] = (raw[8] & 0x3f) | 0x80
	encoded := make([]byte, 36)
	hex.Encode(encoded[0:8], raw[0:4])
	encoded[8] = '-'
	hex.Encode(encoded[9:13], raw[4:6])
	encoded[13] = '-'
	hex.Encode(encoded[14:18], raw[6:8])
	encoded[18] = '-'
	hex.Encode(encoded[19:23], raw[8:10])
	encoded[23] = '-'
	hex.Encode(encoded[24:36], raw[10:16])
	return string(encoded), nil
}

func randomToken(bytes int) (string, error) {
	value := make([]byte, bytes)
	if _, err := rand.Read(value); err != nil {
		return "", err
	}
	return base64.RawURLEncoding.EncodeToString(value), nil
}
