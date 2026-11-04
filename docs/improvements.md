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
