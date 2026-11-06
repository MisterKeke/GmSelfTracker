# GameTracker

GameTracker is a local-first, customizable game progress tracker. You create the games, sections, object types, fields, and goals that make sense to you. The app does not ship game-specific rules or preset characteristics.

The visual design uses an indigo and plum game interface, parchment panels, stepped borders, pixel-style progress bars, and responsive layouts inspired by the supplied retro RPG dashboard.

## Stack

- Go HTTP server and JSON REST API
- SQLite through `database/sql` and the pure-Go `modernc.org/sqlite` driver
- Embedded HTML, CSS, and Vanilla JavaScript modules
- GNU Make for local commands

The Go server serves the frontend and `/api` from one listener. The app does not need Node.js, npm, Docker, a separate frontend server, or a cloud service.

## Run locally

Requirements: Go 1.25 or newer and GNU Make. On Windows, if Make is installed as `mingw32-make`, run that command with the same target.

```text
make dev
```

Open [http://localhost:8080](http://localhost:8080). The first run creates the SQLite database and applies the embedded, versioned migrations. Register with a username and password to start a library.

Useful commands:

```text
make build
make test
make fmt
make vet
make help
```

On the Windows environment used during implementation, GNU Make is available as:

```powershell
mingw32-make dev
```

## Local data and configuration

Default files:

- SQLite database: `data/gametracker.db`
- Uploaded images: `data/uploads/`

The database and uploads are ignored by Git. Set environment variables before `make dev` to override defaults:

| Variable | Default | Purpose |
| --- | --- | --- |
| `HOST` | `127.0.0.1` | Listener address |
| `PORT` | `8080` | Listener port |
| `DB_PATH` | `./data/gametracker.db` | SQLite file path |
| `UPLOAD_DIR` | `./data/uploads` | Local image directory |
| `APP_ENV` | `development` | Set to `production` to mark the session cookie Secure |

`.env.example` documents these values. The app reads environment variables directly and does not require a `.env` file.

## Core concepts

- **Game:** the owner-scoped data boundary.
- **Section:** a custom, nested view that organizes object references. Sections support cards, lists, tables with selectable fields, checklists backed by a user-defined checkbox field, and progress calculated from a progress field, multiple numeric fields, or linked child objects. Child progress can stay direct for existing configurations or explicitly traverse recursive same-type relationships. Goals can be placed in multiple sections with shared progress.
- **Object type:** a user-defined schema such as “Character”, “Location”, or “Project”. These names have no special backend meaning.
- **Field definition:** a typed characteristic attached to an object type. Type changes use a dry-run migration wizard that previews dependencies and converts saved values transactionally. Fields can be archived and restored without losing values.
- **Object:** a game-level record with one object type. Objects are not copied when placed into sections.
- **Object images:** objects can have a primary portrait and a 50-image gallery. Gallery records reuse uploaded assets and protect them from cleanup while referenced.
- **Field value:** data for one object and one stable field ID. Ordinary values are validated by Go and stored as JSON in a relational table. Object references use a separate relational table and are restricted to the same game.
- **Goal:** a game-level manual goal, checklist, numeric tracker, or live rules using numeric, text, select, checkbox, and date fields with all/any conditions.
- **Goal dependency:** a same-game prerequisite edge between goals. Cycles are rejected, and dependent goals cannot be completed while prerequisites remain open.
- **Object preset:** a named set of field overrides over one canonical object. Presets do not replace base values or change goal evaluation.
- **Game template:** a reusable structural copy of selected game sections, object types, fields, and presentation settings, remapped to fresh IDs.

See [docs/architecture.md](docs/architecture.md) for the schema and ownership boundaries.

### Supported field types

Text, long text, integer, decimal, percentage, checkbox, select, date, current/maximum progress, single object reference, multiple object references, and calculated formula.

Calculated formulas are read-only and use a small arithmetic grammar. Reference fields use stable IDs such as `@{a-stable-field-id}`. The evaluator accepts numbers, parentheses, unary signs, addition, subtraction, multiplication, and division. It does not execute JavaScript or user code. Ordinary formula references point to numeric fields on the same object type. An optional relationship aggregation provides `@{related}` using sum, average, minimum, maximum, or count over active linked objects. Aggregation targets ordinary numeric/progress fields; recursive cross-object formulas are excluded.

## Authentication and privacy

Passwords use bcrypt. Authenticated sessions use random opaque IDs stored as hashes in SQLite and HttpOnly, SameSite cookies. State-changing API requests require a CSRF token. Private queries check the current user through the game owner. Registration, login, and password changes share an atomic limit of 10 attempts per client IP per five minutes, including successful attempts, with bounded concurrent password hashing. Browser origin checks protect state-changing endpoints, including login; JSON endpoints require `application/json`. API responses are not cached. Session creation removes expired sessions and retains up to 20 sessions per account. Password changes revoke other sessions in the same transaction. Session creation rechecks the verified password hash within its transaction so a concurrent login cannot issue a session using an obsolete password.

This is a local development application. Production deployment still needs an HTTPS reverse proxy, backups, operational monitoring, and a deployment-specific security review.

## Images

Game covers, game/section/type/field icons, type images, object portraits, dashboard images, and object galleries can use local uploads. Uploads accept PNG, JPEG, GIF, WebP, and BMP up to 5 MB. Images are decoded and normalized to PNG; animated formats use their first frame. Both the input and processed image must fit within 5 MB, 6000 × 6000, and 16 million pixels. Each account can store 1000 images or 250 MB; Profile & Settings lists uploads and allows deletion of unused images. Failed file cleanup stays queued and can be retried from settings; startup and later image operations also retry cleanup. Images referenced by active or archived records, galleries, and dashboards are protected from deletion.

## Example workflow

1. Register, then add a game.
2. Open **Types & Fields** and create an object type.
3. Add the fields you want. Choose field icons from the built-in symbol suggestions or enter another emoji/symbol.
4. Create objects with dynamic forms generated from the type's fields.
5. Add nested sections and place the same object in multiple sections.
6. Create manual goals, checklist goals, numeric progress goals, or field targets.
7. Refresh or restart the app. Records remain in the SQLite file.

## API overview

The API uses JSON and authenticated cookies. Main route groups:

- `/api/auth/*` — register, login, logout, current user, password change
- `/api/games/*` — game library and game-scoped sections, object types, objects, and goals
- `/api/games/{gameID}/overview` — game summary, active object/section counts, and six recently updated objects
- `/api/games/{gameID}/dashboard` — per-game customizable Overview layout
- `/api/games/{gameID}/comparison` — bounded comparison of 2–4 same-type object configurations
- `/api/objects/{objectID}/graph` — bounded, incremental relationship graph expansion from one object
- `/api/objects/{objectID}/gallery` and `/api/object-gallery/*` — gallery records and caption/order/primary controls
- `/api/objects/{objectID}/presets` and `/api/object-presets/*` — reusable object overrides with effective formula values
- `/api/goals/{goalID}/dependencies` — replace/query same-game goal prerequisites with cycle validation
- `/api/templates/*` — save, import/export, duplicate, and instantiate reusable game structures
- `/api/sections/*/placements` — shared many-to-many object placement
- `/api/object-types/*/fields` and `/api/fields/*` — custom field definitions, archive/restore, and safe type migration
- `POST /api/fields/{fieldID}/type-change/preview` and `POST /api/fields/{fieldID}/type-change` — migration dry run and token-guarded apply
- `/api/objects/*/values` — validated values and references
- `/api/objects/{objectID}/history` — paginated object changes, filterable by field, event type, and date
- `/api/goals/*` and `/api/goal-tasks/*` — goal progress and checklist tasks
- `/api/assets/*` — local image uploads and authenticated image reads

Errors use a JSON `error` object containing a stable code and readable message. Object and goal listings support server-side search, filtering, sorting, and pagination with global matching totals.

## Local features

- **Rules:** Create a goal, choose rules based on object fields, and add up to 20 conditions. Choose all or any. Numeric comparisons, date comparisons, text equality/contains, choices, and checkboxes are supported. Missing or archived objects do not satisfy conditions.
- **Relationship calculations:** Create a reference field first, then a formula field. Choose the relationship, operation, and a linked numeric field; use `@{related}` in the expression, optionally alongside ordinary `@{field-id}` references. Count needs no numeric target. Missing numeric values are skipped; empty sums/counts are zero, while empty averages/minimums/maximums show an error.
- **Calculated progress:** Edit a section and select progress view. Choose one progress field, sums of numeric fields from one type (with either maximum fields or a fixed maximum), or linked child progress. Recursive mode is an explicit opt-in, follows one same-type reference field, deduplicates shared leaves by object ID per root, and sums leaf current/maximum values. Parent values are excluded when they have children. Cycles show an error; archived or missing branches are excluded and reported as partial/incomplete.
- **Image icons:** Game, section, type, and field forms accept icon image uploads. A type image also provides a default picture for its object cards.
- **Type lifecycle:** Archive or restore a type in Types & Fields. Its objects are hidden from active views while archived; values and placements are preserved.
- **Goals in sections:** Use “＋ Goal” in a section to place an existing goal. “Remove here” removes only the placement. Goal values remain shared across sections.
- **Object library:** The Objects tab browses all active objects, including those without section placements, with name/description search, type filters, custom filters, and 50-object pages. Section views query their placements directly. Object editors, goal conditions, formula previews, and placement dialogs search the server beyond the current page.
- **Goal library:** Search, status/type filters, sorting, and 20-goal pages are evaluated on the server after current progress is calculated. Condition cards show backend-evaluated values and named fields/objects.
- **Safer editing:** Object metadata and values save together. Editors preserve literal strings and existing archived references, and guard against duplicate submissions. Clearing a field default sends an explicit clear operation.
- **Object history:** Object cards provide a paginated timeline with field, event, and date filters. Numeric field changes can be plotted in a bounded 100-change SVG chart. Creation, metadata/value edits, archive, and restore events preserve stable field IDs and historical labels.
- **Safe field migration:** Types & Fields offers Change Type. Preview covers active and archived objects, defaults, dependencies, and incompatible examples; apply uses a revision token, a bounded transaction, an audit row, and history events. Text/number conversions are exact or explicitly blocked; relationship conversions require ID serialization or an explicit same-game object mapping.
- **Game templates:** Save the current game structure with inclusion options, import/export JSON, duplicate, edit metadata, and create new games from templates. Instantiation remaps IDs and rewrites formulas and references. Templates exclude objects, goals, and private notes.
- **Build presets:** Save alternate per-object field overrides, copy base values, calculate formulas from effective values, and compare presets without mutating the base object. Type migration previews block while preset data still uses the field.
- **Goal dependencies:** Edit prerequisite goals from the goal card. Dependencies are game-scoped, capped, cycle-checked, and enforced for manual, numeric, and final checklist completion.
- **Custom dashboard:** Customize each game's Overview with recent objects, collections, goals, progress, statistics, custom tables, notes, and uploaded images. Reorder by drag and drop or keyboard buttons, resize responsively, and save a per-game layout. Each data widget has independent loading and retry states.
- **Object gallery:** Add up to 50 uploaded images to any object, edit captions and order, select a primary gallery image, and remove links without deleting shared uploads. Archived objects retain readable galleries and block edits.
- **Object comparison:** Compare 2–4 same-type objects, or add another saved configuration of one object. Numeric fields show differences and extrema with neutral-by-default higher/lower preferences; text, choices, booleans, dates, progress, references, and formula errors retain their own display behavior.
- **Relationship graph:** Open a selected object's actual reference network, follow outbound and reverse-discovered edges, expand or collapse branches, zoom and pan, center/select nodes, and open object editors. API expansion is bounded to 100 nodes and 200 edges per response, and cycles deduplicate by object ID.
- **Schema changes:** Removing choices checks active/archived objects, defaults, and goal conditions. The field editor offers replacement mappings; changing allowed reference types requires explicit removal of incompatible existing links.

Existing databases receive pending migrations automatically on the next server start. Migrations 005–011 add object history, audited field-type changes, reusable templates, object presets, goal dependencies, per-game dashboards, and object galleries. Dashboard and gallery image references use the existing asset lifecycle. No migration is applied merely by editing the source.

## Current limits

- Email, password reset, cloud sync, game-service imports, and multi-user sharing remain outside this local-only application.
- Relationship formula aggregation targets ordinary numeric/progress fields, not formulas on other objects. Recursive section progress is limited to 64 relationship levels and 10,000 visited objects per request. Ordinary typed custom-field filters and sorts run in SQLite before object pagination; formula-dependent object queries still evaluate candidates in Go. Goal queries still evaluate live field-target progress across their candidates, with request-local caches for shared objects and field definitions. Checklist progress is batched, and checklist tasks and goal placements are loaded in batches for the returned page.
- New animated image uploads are stored as a static first frame. SVG, AVIF, and HEIC uploads are unsupported.
- Progress view calculations are display settings; they do not overwrite stored object fields.
- Dashboard layouts allow at most 20 widgets and are stored as a bounded JSON layout per game. Collection and goal widgets request only their configured first page.
- Relationship graph reads cap each expansion at 100 nodes, 200 edges, and depth 4; the UI continues from one selected node at a time.

See [the review fixes](docs/review-fixes.md) and [the security review](docs/security-review.md) for findings, fixes, and validation scope.

## Tests

Run:

```text
make test
```

Tests use temporary SQLite databases and in-process HTTP handlers. Coverage includes migrations, authentication, ownership isolation, CSRF/origin checks, shared placements, field lifecycle dependencies, formulas, relationships, multi-condition goal evaluation, paginated queries, recursive progress, type archiving, and image cleanup. Run `go test ./...`, `go vet ./...`, and `node --test web/js/tests/*.test.mjs`. The HTTP query benchmark `BenchmarkObjectQueryPage` builds temporary 1,000, 5,000, and 10,000 object datasets; run it with `go test ./internal/httpapi -run '^$' -bench BenchmarkObjectQueryPage -benchtime=1x`.
