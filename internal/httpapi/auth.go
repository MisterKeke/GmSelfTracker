package httpapi

import (
	"crypto/sha256"
	"crypto/subtle"
	"encoding/hex"
	"net"
	"net/http"
	"regexp"
	"strings"
	"sync"
	"time"

	"golang.org/x/crypto/bcrypt"
)

var usernamePattern = regexp.MustCompile(`^[A-Za-z0-9_-]{3,32}$`)

type loginAttempt struct {
	window time.Time
	count  int
}

type loginLimiter struct {
	mu       sync.Mutex
	attempts map[string]loginAttempt
}

func newLoginLimiter() *loginLimiter { return &loginLimiter{attempts: make(map[string]loginAttempt)} }

func (l *loginLimiter) allowed(key string) bool {
	l.mu.Lock()
	defer l.mu.Unlock()
	now := time.Now()
	attempt := l.attempts[key]
	if now.Sub(attempt.window) >= 5*time.Minute {
		attempt = loginAttempt{window: now}
	}
	return attempt.count < 10
}

func (l *loginLimiter) fail(key string) {
	l.mu.Lock()
	defer l.mu.Unlock()
	now := time.Now()
	attempt := l.attempts[key]
	if now.Sub(attempt.window) >= 5*time.Minute {
		attempt = loginAttempt{window: now}
	}
	attempt.count++
	l.attempts[key] = attempt
}

func (l *loginLimiter) reset(key string) {
	l.mu.Lock()
	delete(l.attempts, key)
	l.mu.Unlock()
}

func (a *API) register(w http.ResponseWriter, r *http.Request) {
	var input struct {
		Username string `json:"username"`
		Password string `json:"password"`
		Confirm  string `json:"confirm_password"`
	}
	if !decodeJSON(w, r, &input) {
		return
	}
	input.Username = strings.TrimSpace(input.Username)
	if !usernamePattern.MatchString(input.Username) {
		writeError(w, http.StatusBadRequest, "invalid_username", "Use 3–32 letters, numbers, underscores, or hyphens for your username.")
		return
	}
	if len(input.Password) < 10 || len(input.Password) > 72 {
		writeError(w, http.StatusBadRequest, "invalid_password", "Your password must be 10–72 bytes long.")
		return
	}
	if input.Password != input.Confirm {
		writeError(w, http.StatusBadRequest, "password_mismatch", "The password confirmation does not match.")
		return
	}
	hash, err := bcrypt.GenerateFromPassword([]byte(input.Password), bcrypt.DefaultCost)
	if err != nil {
		writeError(w, http.StatusInternalServerError, "registration_failed", "Could not create your account.")
		return
	}
	id, err := randomID()
	if err != nil {
		writeError(w, http.StatusInternalServerError, "registration_failed", "Could not create your account.")
		return
	}
	_, err = a.db.ExecContext(r.Context(), `INSERT INTO users(id, username, password_hash, created_at) VALUES(?,?,?,?)`, id, input.Username, string(hash), time.Now().UTC().Format(time.RFC3339))
	if err != nil {
		if strings.Contains(strings.ToLower(err.Error()), "unique") {
			writeError(w, http.StatusConflict, "username_taken", "That username is already in use.")
			return
		}
		a.logger.Error("create user", "error", err)
		writeError(w, http.StatusInternalServerError, "registration_failed", "Could not create your account.")
		return
	}
	user, token, err := a.newSession(r.Context(), id)
	if err != nil {
		writeError(w, http.StatusInternalServerError, "session_failed", "Your account was created, but a session could not be started.")
		return
	}
	expires := time.Now().UTC().Add(30 * 24 * time.Hour)
	a.setSessionCookie(w, token, expires)
	writeJSON(w, http.StatusCreated, user)
}

func (a *API) login(w http.ResponseWriter, r *http.Request) {
	key := clientIP(r)
	if !a.limiter.allowed(key) {
		w.Header().Set("Retry-After", "300")
		writeError(w, http.StatusTooManyRequests, "rate_limited", "Too many login attempts. Try again in a few minutes.")
		return
	}
	var input struct {
		Username string `json:"username"`
		Password string `json:"password"`
	}
	if !decodeJSON(w, r, &input) {
		return
	}
	var id, passwordHash string
	err := a.db.QueryRowContext(r.Context(), `SELECT id, password_hash FROM users WHERE username = ? COLLATE NOCASE`, strings.TrimSpace(input.Username)).Scan(&id, &passwordHash)
	if err != nil || bcrypt.CompareHashAndPassword([]byte(passwordHash), []byte(input.Password)) != nil {
		a.limiter.fail(key)
		writeError(w, http.StatusUnauthorized, "invalid_credentials", "Username or password is incorrect.")
		return
	}
	a.limiter.reset(key)
	user, token, err := a.newSession(r.Context(), id)
	if err != nil {
		writeError(w, http.StatusInternalServerError, "login_failed", "Could not start a session.")
		return
	}
	expires := time.Now().UTC().Add(30 * 24 * time.Hour)
	a.setSessionCookie(w, token, expires)
	writeJSON(w, http.StatusOK, user)
}

func (a *API) logout(w http.ResponseWriter, r *http.Request) {
	if cookie, err := r.Cookie("gametracker_session"); err == nil {
		_, _ = a.db.ExecContext(r.Context(), `DELETE FROM sessions WHERE token_hash = ?`, tokenHash(cookie.Value))
	}
	a.clearSessionCookie(w)
	w.WriteHeader(http.StatusNoContent)
}

func (a *API) me(w http.ResponseWriter, r *http.Request) {
	writeJSON(w, http.StatusOK, a.currentUser(r))
}

func (a *API) changePassword(w http.ResponseWriter, r *http.Request) {
	var input struct {
		CurrentPassword string `json:"current_password"`
		Password        string `json:"password"`
		Confirm         string `json:"confirm_password"`
	}
	if !decodeJSON(w, r, &input) {
		return
	}
	if len(input.Password) < 10 || len(input.Password) > 72 {
		writeError(w, http.StatusBadRequest, "invalid_password", "Your new password must be 10–72 bytes long.")
		return
	}
	if input.Password != input.Confirm {
		writeError(w, http.StatusBadRequest, "password_mismatch", "The password confirmation does not match.")
		return
	}
	user := a.currentUser(r)
	var currentHash string
	if err := a.db.QueryRowContext(r.Context(), `SELECT password_hash FROM users WHERE id=?`, user.ID).Scan(&currentHash); err != nil || bcrypt.CompareHashAndPassword([]byte(currentHash), []byte(input.CurrentPassword)) != nil {
		writeError(w, http.StatusUnauthorized, "invalid_current_password", "Current password is incorrect.")
		return
	}
	hash, err := bcrypt.GenerateFromPassword([]byte(input.Password), bcrypt.DefaultCost)
	if err != nil {
		writeError(w, http.StatusInternalServerError, "password_update_failed", "Could not update password.")
		return
	}
	if _, err := a.db.ExecContext(r.Context(), `UPDATE users SET password_hash=? WHERE id=?`, string(hash), user.ID); err != nil {
		writeError(w, http.StatusInternalServerError, "password_update_failed", "Could not update password.")
		return
	}
	if cookie, err := r.Cookie("gametracker_session"); err == nil {
		_, _ = a.db.ExecContext(r.Context(), `DELETE FROM sessions WHERE user_id=? AND token_hash<>?`, user.ID, tokenHash(cookie.Value))
	}
	w.WriteHeader(http.StatusNoContent)
}

func tokenHash(token string) string {
	sum := sha256.Sum256([]byte(token))
	return hex.EncodeToString(sum[:])
}

func constantTimeEqual(a, b string) bool {
	return len(a) == len(b) && subtle.ConstantTimeCompare([]byte(a), []byte(b)) == 1
}

func clientIP(r *http.Request) string {
	ip, _, err := net.SplitHostPort(r.RemoteAddr)
	if err != nil {
		return r.RemoteAddr
	}
	return ip
}
