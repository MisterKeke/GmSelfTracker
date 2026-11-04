# Security source review — 2026-11-04

This review covered the Go API, authentication/session handling, SQLite ownership boundaries, image uploads, formula evaluation, and browser rendering. Findings below were addressed in source. The server, browser, builds, test suite, and vulnerability scanner were intentionally not run, as requested. Formatting and manual source review are the only validation performed; this is not a production security certification.

| Finding | Change |
| --- | --- |
| Login/register accepted JSON without enforcing its media type, and had no origin policy. A cross-site form could attempt login CSRF. | JSON media types are required. State-changing API requests reject foreign origins and cross-origin Fetch Metadata. Existing authenticated CSRF token checks remain in place. |
| Login limiting used separate check/increment operations, permitting concurrent bypass; registration and password hashing were unbounded. | Reserve attempts atomically across registration/login/password change, retain at most 10,000 IP entries, and allow four concurrent authentication operations. Successful requests also consume capacity. |
| Unknown usernames skipped bcrypt work. | Compare against a fixed dummy bcrypt hash for unknown accounts. Credential lengths are bounded. |
| Password changes updated the hash separately from revoking sessions, ignoring revocation errors. | Compare-and-swap password update and other-session revocation commit in one transaction. Concurrent password changes cannot overwrite each other silently. |
| Expired and repeated sessions accumulated. | Remove expired sessions and retain up to 20 sessions per account on session creation. |
| Field-target goals checked the game but not whether a field belonged to the chosen object's type. Non-field goals could carry arbitrary field references. | Validate the precise object/field/type relationship and active state; clear irrelevant references on other goal types. Migration 002 also removes those invalid links from existing goals while retaining the goal records. New rule conditions repeat the ownership/type validation individually. |
| Dependency checks scanned formulas and sections globally. | Scope dependency names to the field's game, including new aggregation and rule dependencies. |
| Image validation checked only headers; multipart temporary files were not explicitly removed. | Decode pixels and normalize new uploads to PNG, enforce encoded/decoded bounds and two concurrent processors, and remove multipart temporary files. |
| Image sources were unrestricted strings. | Validate HTTP(S) URLs and account-owned upload references before persistence. Arbitrary local endpoints and executable schemes are rejected. |
| Upload storage grew without a per-account bound and game deletion orphaned files. | Enforce an atomic 250 MB / 1000 image quota. Preserve account asset metadata across game deletion. Profile allows deleting unused images, while active and archived references are protected. |
| Formula expressions could consume excessive parser/dependency resources or produce non-finite JSON numbers. | Bound expression length and dependency traversal; reject non-finite results. Relationship aggregation avoids recursive formula targets. |
| New SQLite files relied on the process umask for privacy. | Create new databases with mode 0600 and new data/upload directories with mode 0700 on platforms that honor POSIX modes. Existing permissions and Windows ACLs are not rewritten. |
| Browser security headers and private-response cache policy were incomplete; HTTP bodies could hold connections indefinitely. | Set CSP, frame denial, MIME sniffing prevention, referrer and permissions policies, same-origin resource policy, API/image no-store, production HSTS, and request/write timeouts plus a header bound. Remove the inline retry handler to comply with CSP. |

Origin/media-type handling follows the [OWASP CSRF Prevention Cheat Sheet](https://cheatsheetseries.owasp.org/cheatsheets/Cross-Site_Request_Forgery_Prevention_Cheat_Sheet.html), including protecting login and using Fetch Metadata alongside existing session tokens.

## Operational boundaries

- The default listener remains localhost. Production requires HTTPS and `APP_ENV=production`. If a proxy terminates TLS, preserve the public Host header; untrusted forwarding headers are not used to determine the client IP or origin.
- Rate limiting is per process and direct peer IP. A reverse proxy therefore shares one bucket unless a deployment-specific trusted-proxy design is added.
- External HTTP(S) image URLs remain supported and are fetched by the browser, not by the server. The referrer policy suppresses referrer data; image hosts can still observe requests.
- This pass did not audit dependency advisories or replace the deployment-specific production review. The image dependency is pinned to a version compatible with the existing Go 1.25 requirement.
- New animated uploads use their first frame. Existing uploads are not rewritten by this change.

## Prepared regression coverage

`internal/httpapi/security_features_test.go` covers authentication origin/media types, concurrent rate reservations, image URL policy, aggregation, mixed goal conditions, goal placements, mismatched goal fields, and type archive/restore. The existing migration expectation and PNG fixture were updated for the new migration and complete pixel decoding. These tests have not been executed.

## Follow-up implementation

The subsequent logic review identified an old-password login race, editor round-trip corruption, archived relationship loss, duplicate submissions, partial object saves, and ineffective default clearing. These have source fixes described in [review-fixes.md](review-fixes.md). Session creation now verifies the previously checked password hash inside its insertion transaction. Stable asset references and durable cleanup replace URL-string usage scans and filesystem-first deletion. Additional regression tests were prepared but not executed.

## Subsequent workspace verification

The earlier statements about unexecuted tests describe that source-review pass. The configurable-workspace improvement pass subsequently executed the full Go tests, `go vet`, JavaScript regressions, server builds and isolated browser checks. It retained the ownership/CSRF/upload protections and added ownership-checked section moves, read-only formula previews, typed saved-view validation, primary-field validation and transactional goal editing. Details and remaining verification limits are in [improvements.md](improvements.md); this does not constitute a dependency vulnerability scan or a production security certification.
