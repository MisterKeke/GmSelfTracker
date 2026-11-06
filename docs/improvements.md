# Configurable workspace improvements — 2026-11-04

## Scope and audit

This work extends the existing Go + SQLite + vanilla JavaScript application. The single HTTP listener, embedded assets, authentication, pixel palette, stable IDs, uploads, and game-level objects remain in place. Pre-existing uncommitted review/security changes were preserved.

| Area inspected | Existing behavior reused | Change |
| --- | --- | --- |
| `cmd/server/main.go`, `Makefile` | Embedded frontend and API on one listener; `make dev` uses port 8080 | Retained; optional `test-js` target added |
| `internal/database`, migrations 001–003 | Versioned transactional migrations; placements separate from objects | Additive migration 004; previous-schema upgrade regression |
| `internal/httpapi/sections.go` | Parent IDs, ordering, cycle checks, archive/restore, display JSON | Atomic move endpoint, saved-view validation, hierarchical UI |
| `internal/formula`, formula/aggregation handlers | Arithmetic parser, stable field references, cycle detection, related-object aggregation | Shared syntax validation and read-only example preview |
| `internal/httpapi/goals.go`, `goal_rules.go` | Four goal types, task IDs, ALL/ANY rules and section placements | Transactional edits and initial checklist completion states |
| Object/type handlers | Typed values, field archiving, ownership and reference validation | Optional primary display reference; creation dates in object responses |
| `web/js/workspace.js`, API/auth/modal modules, `app.css` | Existing events, editors, CSRF handling, escaped HTML, retro styling | Focused modules, visual formula editor, filters, tree navigation, safer modal workflows |
| Architecture/security docs and existing tests | Existing security and lifecycle regression coverage | Retained and run alongside the new regression tests |

## Implemented behavior

- Workspace state, navigation, events, mutations, components, content views, goal views, and editors are separate ES modules. `workspace.js` remains a compatibility entry point. A private composition context connects callbacks without circular imports or state on `window`.
- Sections have an indented, expandable tree with custom icons, selection highlighting, drag grips, insertion targets, and root drops. Row edges reorder; row centers nest. Move dialogs offer parent/position selection and up/down/root actions. Moves retain descendants and placements.
- Archiving retains the existing policy: only the chosen section is archived. Active children remain accessible at the tree root while their parent is archived. Their original parent IDs and order are preserved and reappear on restoration. The archive prompt explains this behavior. Objects and goals are untouched.
- Formulas can be assembled from readable field buttons, operators, parentheses and constants. Stable IDs remain the stored representation. Advanced text editing remains available. The existing Go parser validates and calculates previews using an existing object or temporary numeric inputs. Relationship selectors preserve sum, average, minimum, maximum and count. Preview writes no data and reports missing values and calculation errors.
- Object search includes name, type and description. Typed conditions support numeric, text, select, boolean, date, progress and reference fields, with ALL/ANY matching. Selecting a type scopes custom fields. Sorting supports names, types, creation/update dates and compatible fields; missing values are last in both directions. Section views save into existing display configuration, without changing values. Section editing preserves saved query settings.
- Goal editing supports general properties, manual completion, numeric current/target values, checklist task changes and ordering, and field-linked ALL/ANY rules. Existing task/goal IDs, creation times and placements are preserved. Fundamental goal types remain immutable; calculated progress cannot be manually overwritten.
- Object types can choose, change or clear a primary display field by ID. Cards respect section-visible fields. Archived or incompatible selections use the normal card fallback; no field meaning is inferred from a name.
- Modals support Escape, focus restoration, a keyboard-accessible discard warning, submission locking, in-place errors and retained form values. Authentication errors retain entered values. Icon palettes supplement symbols and uploads. Reference inputs have local search. Empty defaults remain empty while zero, false and structured defaults round-trip correctly.

## Files changed by this implementation

Modified:

- `Makefile`
- `internal/database/database_test.go`
- `internal/formula/formula.go`, `formula_test.go`
- `internal/httpapi/api.go`, `goals.go`, `objects.go`, `sections.go`, `types.go`
- `web/js/workspace.js`, `auth.js`, `modal.js`
- `web/css/app.css`
- `docs/architecture.md`, `docs/security-review.md`

Added:

- `internal/database/migrations/004_primary_display.sql`
- `internal/httpapi/section_moves.go`, `formula_preview.go`, `object_views.go`, `goal_updates.go`, `improvements_test.go`
- `web/js/workspace/index.js`, `state.js`, `navigation.js`, `events.js`, `actions.js`
- `web/js/components/section-tree.js`, `object-browser.js`, `object-views.js`
- `web/js/views/content.js`, `goals.js`
- `web/js/editors/game-editor.js`, `field-editor.js`, `formula-editor.js`, `object-editor.js`, `goal-editor.js`, `account-editor.js`
- `web/js/utils/formatting.js`, `object-query.js`, `section-tree-model.js`
- `web/js/tests/object-query.test.mjs`, `display.test.mjs`
- `docs/improvements.md`

## Database and API compatibility

Migration **004** adds `object_types.primary_display_field_id TEXT NOT NULL DEFAULT ''`, with insert/update triggers enforcing type membership. Old records have no primary selection. Archived selected fields retain their IDs and values. No tables, uploads, values, goal IDs, or object IDs are removed. The upgrade test creates a version-003 database with archived values and placements, applies the new migration, checks preserved data and foreign keys, and repeats migration application.

New endpoints:

- `POST /api/sections/{sectionID}/move`: `{parent_id: ID|null, before_id?: ID}`. Moves before a destination sibling, or appends when omitted. Ownership, game, active parent, self/descendant and destination-sibling checks precede a transactional order update.
- `POST /api/object-types/{typeID}/formula-preview`: `{field_id?, object_id?, options, values?, related?}`. Returns schema validation errors, or `{valid:true, result}` / `{valid:true, error}` for example evaluation. Existing formula schema/aggregation checks remain authoritative.

Extended endpoints:

- Object-type GET/list responses include `primary_display_field_id`; type PATCH accepts it, with `''` clearing the selection.
- Goal PATCH accepts `target`, `conditions`, `mode`, legacy `object_id`/`field_id`, and a checklist `tasks` array of `{id?, title, completed}`. Array order becomes task order. Omitted tasks are retained; omitted IDs within an explicitly submitted task list are removed. New tasks receive generated IDs.
- Goal creation retains legacy string `tasks`; optional `task_details: [{title, completed}]` saves initial task states in the same transaction. Manual creation also accepts `completed`.
- Section `display_config.object_view` stores `{search, type_id, mode, conditions, sort, direction}`. A condition uses `{field_id, operator, value?, value_to?}`. Existing display settings remain compatible.
- Object responses include `created_at` for sorting.

No endpoint was removed. Existing saved formulas and scalar field-target goals remain supported; editing a scalar target can convert its representation to an equivalent stable-ID rule while retaining the goal record.

## Verification

GNU Make was available as `D:\Qt\Tools\mingw1310_64\bin\mingw32-make.exe`, though `make` was not on this shell's PATH. The actual targets were executed through that binary:

```text
mingw32-make.exe test vet test-js build
go test ./...
go vet ./...
node --test web/js/tests/*.test.mjs
node --check <each frontend JavaScript module>
mingw32-make.exe dev
git diff --check
```

Final results: all Go packages passed; `go vet` reported no diagnostics; all **12 JavaScript tests passed**; every frontend module passed syntax checking; the Windows executable built successfully; `git diff --check` found no whitespace errors. The real `dev` target served the application at `http://localhost:8080`. Test-only servers, binaries, database, and uploads were removed after verification.

Backend regressions cover transactional moves, cycles, cross-game/user protection, archive/restore placement preservation, general/manual/numeric/checklist/rule edits, task identity/order and rollback, ALL/ANY recalculation, legacy goals, primary configuration, previous-schema upgrades, formula syntax/renaming/cycles/missing values/division by zero, all existing aggregation operations, and saved filters for every field family.

Frontend unit regressions cover typed comparisons, search, mixed types, ALL/ANY, missing values, stable sorting, archived fields, formula errors, tree ordering/expansion and 300 nesting levels, primary rendering/escaping/section visibility, and empty-default round trips.

Browser verification used a separate disposable SQLite database and upload directory. The user's `data/` database and uploads were not opened for tests. The browser exercised registration and fresh login, game creation/switching, nested section creation/editing/moves, drag nesting/root moves/sibling ordering, keyboard tree navigation and remembered expansion, custom type/field creation and field renaming, object creation/editing/upload, formula creation/editing/validation and object-based examples, filtering/sorting/saved views after refresh, checklist and rule goal creation/editing, primary selection, field archive/restore, modal reopening/Escape/discard/recovery, and persistence across a Go-server restart. The final empty-default fix was verified by reopening an existing field with a null default. Numeric/manual goal editing and the less common filter families are covered by automated API/unit tests rather than exhaustive browser runs.

## Limits

- Filtering and sorting use the already paginated-and-hydrated local collection. Filter interactions do not refetch it; the library renders 50 results per page. Very large collections still load into browser memory initially; no large-scale benchmark or new backend query engine was added.
- Goal types remain immutable. Checklists require at least one task; goal rules and object filters allow at most 20 conditions, matching the existing bounded goal model.
- Branch and navigation preferences use per-user, local browser storage; saved section query settings use SQLite. Unsaved query changes survive ordinary navigation within the workspace but are not committed until Save section view is used.
- No full assistive-technology audit or cross-browser/device matrix was performed. Existing reduced-motion and focus styling are retained and extended. No dependency vulnerability scan was run during this task.

## Follow-up: dependency safety, recursive progress, and paged collections

This section records the 2026-11 follow-up. It extends the earlier implementation above; historical descriptions of local-only collection filtering refer to that earlier state.

### Field and upload lifecycle

Field archive checks now run in the same SQLite transaction as the archive update. They parse saved section display configuration and report dependencies from active or archived sections, including object-view filters, sorting, selected fields, checklist fields, and progress calculations. Formula dependency inspection includes transitive formulas and remains scoped to the field's game. The API blocks archive rather than clearing a user's configuration or values. No migration was needed.

An upload performed during a form save is tracked by that operation. If the resource save fails, the frontend requests deletion for each new asset and leaves entered form values available for retry. The server continues to enforce asset references, including archived records, so compensation cannot delete a concurrently reused image. Cleanup errors stay visible; stale unreferenced uploads are queued after 24 hours through existing asset cleanup. If an upload response is lost before the browser receives its asset ID, the age-based queue is the recovery path.

### Object queries

`GET /api/games/{gameID}/objects` accepts `limit`, `offset`, `search`, `type_id`, `section_id`, `exclude_section_id`, `exclude_object_id`, `sort`, `direction`, and a JSON `filters` object. Results include `items`, matching `total`, full active `game_total`, `limit`, `offset`, and `has_more`. Search covers object name, description, and type name case-insensitively. Name, type, creation, and update sorts happen in SQL before pagination. Custom field filtering/sorting supports existing typed field operators, including formulas evaluated by Go; those candidate sets are hydrated in batches and sorted/filtered before slicing, so memory use still grows with the candidate set. Ties use object ID and missing values sort last.

The workspace requests a page at a time, retains a bounded object detail cache, and uses backend search for references, formula examples, goal condition objects, and section placement. Section contents query the placement join directly. Object and goal page requests guard against stale results when the user switches context.

### Goals

`GET /api/games/{gameID}/goals` accepts `limit`, `offset`, `search`, `status`, `type`, `sort`, `direction`, `section_id`, and `exclude_section_id`. Filtering uses server-evaluated completion and progress, then sorting and pagination run before the response. The response includes matching `total`, unfiltered `game_total`, evaluated `completed_total`, and pagination metadata. Search is case-insensitive over title and description. Supported sort keys are title, creation/update time, progress, and completion; ties use stable goal IDs. The unparameterized endpoint remains backward compatible and returns the full list. The workspace loads only a three-goal overview, the current section's placements, or the requested 20-goal page. A single-goal GET supports on-demand editor data. Current progress evaluation retains request-local object/field caches.

For field-target goals, each condition response includes its stable object/field IDs and names, operator, target, current value when available, match result, and a state such as `satisfied`, `not_satisfied`, `missing_value`, `invalid_value`, `field_unavailable`, or `object_unavailable`. The UI displays the server's ALL/ANY rule and per-condition state.

### Recursive linked progress

Section display configuration may opt into `progress_depth: "recursive"` alongside `progress_mode: "children"`, `progress_relationship_field_id`, and `progress_child_field_id`. Existing or absent depth settings use direct-child behavior. Recursive mode follows only the selected relationship and requires it to accept the same object type. Nodes with active children do not contribute their own progress; only unique leaf IDs contribute, summing current and maximum rather than averaging percentages. Shared leaves count once per root. A cycle produces a named path and error status with no percentage. Archived/unavailable descendants and invalid progress values are excluded with warnings; a result with both valid leaves and warnings is marked partial, and no valid maximum is incomplete. Traversal batches relationship/value reads and caps at 64 levels and 10,000 objects. It does not add a geographic or other domain-specific model.

### Verification and performance

`TestObjectQueriesReturnGloballySortedFilteredPagesAndTotals`, `TestGoalQueriesFilterSortAndPaginateBeforeReturningResults`, `TestSectionProgressDirectRecursiveDeduplicationCyclesAndArchivedChildren`, field archive tests, and stale-upload cleanup tests cover the main follow-up paths. JavaScript API, goal-query, display, and section-tree tests remain part of `node --test web/js/tests/*.test.mjs`. `BenchmarkObjectQueryPage` builds isolated temporary datasets of 1,000, 5,000, and 10,000 objects and measures a page read, a name search, and custom-field filter/sort. Run it with:

```text
go test ./internal/httpapi -run '^$' -bench BenchmarkObjectQueryPage -benchtime=1x
```

The benchmark reports measured durations and allocations; results depend on the local machine and are not represented as a general speedup claim. The Go handler still evaluates dynamic goal progress before sorting/filtering, and custom-field object queries process the candidate collection in server memory. Normal browser navigation no longer downloads the full object library or full game goal collection.

On a Windows 11 machine with an Intel Core i5-12500H, the latest one-iteration sample was: for 10,000 objects, a 50-item base page took 26.2 ms and allocated 509 KB; a name search took 43.3 ms and allocated 139 KB; custom numeric filter/sort took 155.2 ms and allocated 98.2 MB. At 5,000 objects, custom filter/sort took 75.5 ms and allocated 48.6 MB; at 1,000 it took 14.9 ms and allocated 9.6 MB. These are single-iteration diagnostics, not statistically stable throughput claims. The custom-field path is still the most expensive query and is a clear target for future streaming/value-index work.

The follow-up's real-browser E2E pass could not be completed in this environment. `mingw32-make dev` started successfully with a disposable database, but the computer-use approval layer rejected opening Google Chrome, and the shell sandbox denied a loopback HTTP request. The server was stopped and the temporary database/upload directory was removed. The Go in-process HTTP tests and frontend unit/syntax checks did run; no claim is made that the new flows were visually exercised in a browser.

## Overview data source — 2026-11-05

Overview no longer derives its recent-object cards from the current paginated object query. The authenticated `GET /api/games/{gameID}/overview` endpoint returns active object and section counts plus at most six active objects ordered by `updated_at DESC, id ASC`. It hydrates only those six records, so section filters, saved object views, current page, and library sort cannot change the Overview list. Existing goal listing continues to supply three recently updated goals and game-wide goal/completion totals after live progress evaluation. Overview requests are game-scoped and sequence-guarded; the UI distinguishes loading, a truly empty game, an unavailable recent list, and a retryable request error. This additive endpoint needs no SQLite migration.

## Typed object query execution — 2026-11-05

Compatible custom-field predicates and sorts now run in SQLite before pagination for integer, decimal, percentage, text/long text, select, boolean, date, and progress values. Query construction uses validated field IDs, a fixed operator map, JSON type checks, and bound filter values. Missing object values use the field's configured default, matching hydration behavior. Sorting leaves missing/invalid values last in either direction and uses object ID as a stable tie-breaker. Formula evaluation is never moved into SQL; formula-dependent queries and reference filters continue through the existing full-candidate Go evaluator. `EXPLAIN QUERY PLAN` confirms the correlated value lookup uses the existing field-value primary-key index, so no new index or migration was required. Global totals and filtering-before-pagination remain unchanged. One regression fix also prevents `one_of` filter validation from overwriting the original JSON array while decoding its members.

The existing `BenchmarkObjectQueryPage` was run before and after the query change on Windows 11 with an Intel Core i5-12500H. The pre-change diagnostic was a single iteration; the post-change sample used three iterations, so timing comparisons are indicative only. For 10,000 objects, custom numeric filter/sort measured 413 ms and 98.2 MB allocated before, then 320 ms/op and 2.68 MB/op after. For 5,000, the post-change sample measured 52.0 ms/op and 493 KB/op; for 1,000 it measured 9.30 ms/op and 434 KB/op. The clearest result is lower memory proportional to matching candidates; the timing sample is not a stable general throughput claim.

Goal list reads no longer issue one checklist task query and one placement query per candidate goal. Completion/progress counts are aggregated in batches before filtering and sorting; full task and active-section placement details are hydrated in batches after pagination. Field-target progress remains live, and request-local object/field caching remains in place. The unpaged API response remains complete. No migration was required.

`BenchmarkGoalQueryPage` exercises a page of 20 goals over 1,000 total manual/checklist goals with checklist tasks. A three-iteration Windows sample measured 6.62 ms/op and 1.94 MB/op. This fixture measures checklist batching and page-detail hydration; field-target/formula-heavy workloads were not benchmarked here.

## Object history — 2026-11-05

Migration 005 adds `object_history_events` and `object_history_changes`. Each event stores the game, object ID/name snapshot, authenticated user, server-selected source, event kind, and a fixed nanosecond UTC timestamp. Change rows keep stable field IDs, field-name/type snapshots, and JSON before/after values without foreign keys to field definitions, so a later rename does not rewrite history. Object creation, metadata/value edits, archive, and restore write their history rows in the same transaction as the underlying object mutation. Rejected or rolled-back edits do not leave events.

`GET /api/objects/{objectID}/history` is authenticated and owner-scoped. It returns at most 100 events per page, total/offset metadata, stable change details, and known historical field labels. `event_type`, `field_id`, `since`, and `until` filters are parameterized. The object workspace adds a retro-styled timeline and an optional SVG numeric progression chart capped at the latest 100 matching changes. Only numeric snapshots are charted; values recorded while a field was text are omitted.

API tests cover creation, field and metadata changes, archived/restored objects, immutable field-name snapshots, filters, pagination, failed-write atomicity, and cross-user access. Frontend tests cover the timeline states, escaped values, filters, and numeric-only charting. Browser verification for this new UI was not available in this environment.

## Safe field type migration — 2026-11-05

Migration 006 adds `field_type_migrations` and a trigger that allows a field type update only when the same transaction contains a matching audit row in `applying` state. The normal field PATCH route continues to reject type changes. `POST /api/fields/{fieldID}/type-change/preview` runs a consistent read transaction and returns affected/saved/archived counts, converted defaults, dependencies, incompatible examples, and a revision token. `POST /api/fields/{fieldID}/type-change` recomputes the preview inside its write transaction, rejects a stale token, converts values in bounded batches, records per-object `field_type_migrated` history, updates the field definition, marks the audit applied, and commits atomically.

Conversion behavior is explicit:

| Conversion | Behavior |
|---|---|
| Text ↔ long text | Lossless when the target length limit is met. |
| Integer → decimal; percentage → decimal | Preserves numeric meaning. |
| Decimal → integer | Only mathematically integral values pass; no rounding. |
| Text → integer/decimal/percentage | Only valid numeric representations pass. |
| Integer/decimal/text → percentage | Enforces 0–100; an explicit clamp policy may clamp to an endpoint. |
| Numeric → text | Exact canonical decimal text; booleans become `true`/`false`. |
| Text → boolean/date/select | Requires exact true/false, canonical ISO date, or a listed choice/replacement. |
| Numeric → progress | Requires an explicit positive maximum. |
| Progress → numeric | Requires an explicit current, maximum, or percentage strategy. |
| Formula → numeric | Requires freeze confirmation and stores the current calculated result for each object. |
| Reference → text/long text | Requires explicit JSON serialization of linked IDs. |
| Text/long text/select → reference/references | Requires an explicit mapping to active objects owned by the same game and matching any allowed type. |
| Single ↔ multiple reference | Preserves links when cardinality is compatible; multiple links block conversion to single. |

Formula, goal, primary display, section-view, and relationship dependencies are inspected. Cross-family changes are blocked while incompatible dependencies remain. Unsupported conversions and invalid values stay blocked with examples rather than being rounded, truncated, or discarded. Preview/apply tests cover conversion rules, stale previews, ownership, archived values, formula freezing, reference serialization/mapping, the database guard, audit rows, and history events. The UI wizard is wired into Types & Fields; browser visual verification still requires the unavailable computer-use approval path.

## Game templates — migration 007

Templates save selected game structure: sections, object types, field definitions/defaults, and compatible presentation settings. They exclude objects, goals, and private game notes. The private template library supports create, inspect, import/export JSON, rename, duplicate, delete, and instantiate. Instantiation creates a new game transactionally with fresh IDs and remaps section parents, allowed type IDs, formula references, and view configuration. A per-user limit of 100 templates is checked for saves, imports, and copies.

`POST /api/games/{gameID}/templates`, `/api/templates/*`, and `POST /api/templates/{templateID}/instantiate` are owner-scoped. Tests cover export/import validation, ID remapping, quota limits, and user isolation. Dashboard layouts are independent and are not included in templates.

## Object presets — migration 008

`object_presets` stores up to 100 named JSON overrides per object. A preset read merges overrides with the current canonical values and recalculates formulas, including relationship aggregations. Presets never write to base `field_values` and do not alter goals. The UI supports empty presets, copied base snapshots, edits, duplicates, deletion, and switching the displayed base/effective configuration. Preset creation and changes are added to object history.

The field migration preview counts preset override use; apply is blocked until those overrides are removed. Tests cover owner/game scoping, value validation, formula recalculation, no canonical mutation, preset history, limits, and migration guards.

## Goal dependencies — migration 009

`goal_dependencies` stores directed prerequisite edges within one game. `GET` and `PUT /api/goals/{goalID}/dependencies` reject foreign-game goals, self-links, duplicate edges, over-limit lists, and cycles. Completion guards cover manual and numeric goal updates and the final checklist task. Goal cards show readiness and provide a dependency editor; removing or completing a prerequisite refreshes the dependent status.

Tests cover cycle rejection, ownership, cascade cleanup, blockers, and completing goals after prerequisites. Dependencies affect completion only; they do not change the goal's underlying field target or checklist data.

## Custom dashboard — migration 010

`game_dashboards` stores a versioned JSON layout per game, capped at 64 KB and 20 widgets. The default layout presents recent objects, statistics, and active goals. The editor can add/configure/remove/reorder/resize widgets and reset the layout; layout changes stay in a draft until saved. Drag and drop is paired with keyboard move buttons. Responsive sizes use CSS grid.

Widget types include recent objects, type/section collections, goals, progress, statistics, custom tables, plain-text notes, and uploaded images. Object and goal widgets fetch only their configured page, maintain independent loading/error states, and retry independently. Saved source references are retained when a source is archived; the affected widget reports the API error and remains configurable. Dashboard image assets participate in `asset_references` within the save transaction. API regressions cover defaults, persistence, ownership, malformed layouts, source validation, image reference protection, and per-game scope.

## Object gallery — migration 011

`object_gallery` rows point to existing uploaded asset IDs; image bytes are not duplicated. An object can have up to 50 gallery entries with captions, stable order values, and one optional primary marker. The gallery modal supports multi-upload, lazy thumbnails, opening full-size images, caption/order editing, choosing the primary image, and removing gallery references. Removing a row only releases that gallery reference; other resource references still protect the asset. Archived object galleries remain readable and immutable until the object/type is restored.

Insert/update/delete triggers maintain asset references and validate that uploads belong to the object's user and remain active. Tests cover uploads, caption/order/primary updates, asset reuse, deletion protection, archived records, cross-user reads, and edit guards. The primary gallery marker stays separate from the main object portrait.

## Object comparison

`POST /api/games/{gameID}/comparison` accepts 2–4 active same-type object/configuration pairs and returns only selected objects plus their active shared fields. It calculates formula values server-side and includes names for referenced objects. The UI lets users select objects in the library or a section, compare their fields, choose a preset per column, and add another saved configuration of the same object for base-vs-preset or preset-vs-preset comparisons.

Numeric fields show absolute and (when defined) percentage differences plus highest/lowest values. Preference is neutral by default, with optional higher/lower settings; no automatic “higher is better” colors are used. Other types retain their formatted values, references resolve to names, and formula errors are displayed. API tests cover same-type enforcement, selected preset values, foreign IDs, and permissions.

## Relationship graph

`GET /api/objects/{objectID}/graph` performs bounded breadth-first expansion from one selected object. Query options select depth (1–4), direction (`outbound` or `both`), and a node cap (2–100); responses cap edges at 200. Nodes deduplicate by stable object ID, edges preserve the actual stored source/target direction and field label, and reverse-discovered edges are marked. Archived nodes stay visible but are not expanded.

The workspace graph is an SVG view with arrowed edges, node selection, object opening, on-demand expansion, branch collapse, zoom, pan, and centering. Each expansion starts at the chosen node instead of fetching a game-wide graph. Tests cover chains, branching, shared nodes, cycles, archived nodes, limits, direction, and cross-user/game isolation.

## Cross-feature verification

Migrations 005–011 are applied by the existing ordered migration runner. Focused API tests cover dashboard, gallery, comparison, and graph flows alongside prior history, migration, template, preset, and dependency suites. The final full Go and JavaScript suites are recorded in the implementation report. An authenticated visual browser pass was not completed because the computer-use approval layer rejected opening Chrome and the shell sandbox denied loopback HTTP requests; this limitation does not affect the in-process API and frontend unit test results.
