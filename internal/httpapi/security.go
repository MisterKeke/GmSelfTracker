package httpapi

import (
	"mime"
	"net/http"
	"net/url"
	"strings"
)

// Apply this outside the router so errors, static files and authentication share
// the same browser policy. Origin checks also protect unauthenticated login.
func (a *API) security(next http.Handler) http.Handler {
	return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		w.Header().Set("X-Content-Type-Options", "nosniff")
		w.Header().Set("X-Frame-Options", "DENY")
		w.Header().Set("Referrer-Policy", "no-referrer")
		w.Header().Set("Cross-Origin-Resource-Policy", "same-origin")
		w.Header().Set("Permissions-Policy", "camera=(), microphone=(), geolocation=()")
		w.Header().Set("Content-Security-Policy", "default-src 'self'; script-src 'self'; style-src 'self' 'unsafe-inline'; img-src 'self' https: http:; connect-src 'self'; object-src 'none'; base-uri 'none'; frame-ancestors 'none'; form-action 'self'")
		if a.secure {
			w.Header().Set("Strict-Transport-Security", "max-age=31536000")
		}
		if strings.HasPrefix(r.URL.Path, "/api/") {
			w.Header().Set("Cache-Control", "no-store")
			if r.Method != http.MethodGet && r.Method != http.MethodHead && r.Method != http.MethodOptions {
				origin := r.Header.Get("Origin")
				if origin != "" {
					u, err := url.Parse(origin)
					scheme := "http"
					if r.TLS != nil || a.secure {
						scheme = "https"
					}
					if err != nil || u.Scheme != scheme || !strings.EqualFold(u.Host, r.Host) || u.User != nil || u.Path != "" || u.RawQuery != "" || u.Fragment != "" {
						writeError(w, http.StatusForbidden, "invalid_origin", "Requests must come from this application.")
						return
					}
				}
				if site := r.Header.Get("Sec-Fetch-Site"); site != "" && site != "same-origin" && site != "none" {
					writeError(w, http.StatusForbidden, "invalid_origin", "Requests must come from this application.")
					return
				}
			}
		}
		next.ServeHTTP(w, r)
	})
}

func (a *API) limitAuthentication(next http.HandlerFunc) http.HandlerFunc {
	return func(w http.ResponseWriter, r *http.Request) {
		if !a.limiter.take(clientIP(r)) {
			w.Header().Set("Retry-After", "300")
			writeError(w, http.StatusTooManyRequests, "rate_limited", "Too many authentication attempts. Try again in a few minutes.")
			return
		}
		select {
		case a.authSlots <- struct{}{}:
			defer func() { <-a.authSlots }()
		default:
			w.Header().Set("Retry-After", "5")
			writeError(w, http.StatusTooManyRequests, "server_busy", "Please try again in a moment.")
			return
		}
		next(w, r)
	}
}

func requireJSON(w http.ResponseWriter, r *http.Request) bool {
	mediaType, _, err := mime.ParseMediaType(r.Header.Get("Content-Type"))
	if err != nil || mediaType != "application/json" {
		writeError(w, http.StatusUnsupportedMediaType, "json_required", "Send application/json.")
		return false
	}
	return true
}

// Images are either owner-scoped uploads or ordinary web URLs. Never accept
// arbitrary local endpoints as image sources (for example /api/auth/logout).
func validImageURL(value string) bool {
	if value == "" {
		return true
	}
	if len(value) > 2048 {
		return false
	}
	if strings.HasPrefix(value, "/api/assets/") {
		id := strings.TrimPrefix(value, "/api/assets/")
		return len(id) == 36 && !strings.ContainsAny(id, "/?#%\\")
	}
	u, err := url.Parse(value)
	return err == nil && (u.Scheme == "http" || u.Scheme == "https") && u.Hostname() != "" && u.User == nil
}

func (a *API) validateImage(r *http.Request, value string) bool {
	if !validImageURL(value) {
		return false
	}
	if strings.HasPrefix(value, "/api/assets/") {
		var count int
		err := a.db.QueryRowContext(r.Context(), `SELECT COUNT(*) FROM uploaded_assets WHERE id=? AND user_id=?`, strings.TrimPrefix(value, "/api/assets/"), a.currentUser(r).ID).Scan(&count)
		return err == nil && count == 1
	}
	return true
}
