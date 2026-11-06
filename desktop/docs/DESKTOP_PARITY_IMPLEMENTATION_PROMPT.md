# Complete GmSelfTracker desktop feature parity

Use this prompt in a coding agent with the GmSelfTracker repository open at its root. The task is to implement and verify the workflows below. Begin by inspecting the checkout, then carry the work through code, tests, and documentation.

## Objective and scope

Bring the native C++20 and Qt 6 Widgets app to functional parity with the current Go and vanilla JavaScript app for every workflow in this prompt. Port the current web behavior and data contracts to desktop. Keep existing desktop games usable, make migrated web data usable under its original account, and preserve the native app’s independent build and runtime.

Follow `AGENTS.md`. Use the current checkout and executable code as the baseline. The audit at `5a33d8b` identified the gaps below. Recheck each one and credit existing functionality only when its normal entry point works and its behavior matches the current contract. Keep browser-specific authentication and delivery mechanics scoped to web. If a shared defect or schema change is necessary, trace both paths, make the corresponding changes, and verify both implementations in this task.

Preserve the retro theme, native dialogs, keyboard and focus behavior, and familiar navigation. Keep domain rules and data access outside widgets. Use the existing repository conventions, native SQLite, and Qt Widgets. Keep the Go server, browser, and WebEngine outside the desktop runtime. Keep edits focused on the workflows below and use the existing CMake presets, PowerShell Make wrapper, and portable distribution workflow.

## Establish the baseline

1. Read `AGENTS.md`, `docs/architecture.md`, `desktop/docs/ARCHITECTURE.md`, `desktop/docs/FEATURE_PARITY.md`, `desktop/docs/DATA_IMPORT.md`, `desktop/docs/VISUAL_PARITY.md`, and `desktop/docs/BUILDING.md`. Treat status documentation as a snapshot.
2. Trace the web UI from `web/js/workspace/`, `web/js/editors/`, `web/js/components/`, and `web/js/views/` through the routes in `internal/httpapi/api.go`. Read the corresponding handlers, `internal/formula/`, and relevant Go and JavaScript tests. Inspect the native entry point, `MainWindow`, `GameRepository`, domain records, history widgets, image decoder, and Qt tests.
3. Compare the two migration directories, migration runners, Qt resource registrations, and `schema_migrations` version positions. The audit found matching migrations 001–011. Use the checkout’s actual history when planning changes.
4. Record a short matrix of native entry points, missing behavior, shared data contracts, and tests. Run available baseline suites with disposable data. Then implement the phases in dependency order. Complete vertical slices through UI, validation, persistence, and reload as you go.

## Phase 1. Local accounts, images, and web data import

### 1. Native accounts and existing profiles

- Provide registration, sign-in, sign-out, and password change in native UI. Match web credential validation and bcrypt behavior for credentials produced by either app. Verify compatibility against Go-generated hashes, including the web password byte limit and non-ASCII inputs. Bound expensive authentication attempts and keep hashing off the GUI thread.
- Replace the repository’s fixed local user ID with an authenticated account context. Scope every owned read and write, including asset access, history, search, archive recovery, and the new workflows. Verify game and type boundaries at the repository or service layer even when the UI restricts a picker.
- Preserve pre-existing desktop data. Define and test a deliberate one-time path for securing the legacy local profile under user-chosen credentials while retaining its user ID and all owned rows. Distinguish this exact local profile from imported or already authenticated accounts. Show users which account is active. Signing out or switching accounts clears account-specific state and invalidates in-flight UI results.
- Give desktop the native equivalent of session invalidation if it supports multiple sessions or remembered sign-in. Keep web cookies, CSRF, and active web tokens outside desktop authentication. Store only the credentials and session material required by the existing data contract and the chosen native flow.

### 2. Image storage and management

- Build the native asset store and a Profile & Settings image library. Support upload, selection or reuse, previews, quota display, deletion of unused images, and retry of pending cleanup. Add image controls for game covers, type fallback images, object images, gallery entries, and dashboard image widgets. Match the web’s accepted icon and image-reference forms, use a safe fallback for unavailable images, and preserve saved values when editing unrelated fields.
- Trace `assets.go`, `local_features.go`, and asset-reference migrations. Match supported formats, first-frame handling, byte, pixel, and dimension limits, safe decoding, normalization to static PNG, and current per-account quotas. The audited web limits include 5 MiB per upload, 1,000 files, and 250 MiB per account. Verify limits in current source. Reuse the native decoder and preserve portable WebP support.
- Use generated storage names, verify that resolved paths stay within the owned asset root, and resolve existing shared asset references consistently. Asset references and feature writes must stay consistent on save, archive, restore, and deletion. Keep referenced assets protected across active and archived records. Preserve durable pending deletion, bounded cleanup, and correct quota accounting if file operations fail or are retried.
- Test invalid and oversized files, supported formats, quota exhaustion, cross-account selection and reads, failed saves, archived references, and a failed cleanup followed by a successful retry.

### 3. Import a web database and its uploads

- Implement a native wizard with a source SQLite picker, uploads-folder picker, preflight summary, progress and cancellation, and a final report. Import into a separate desktop data store. Design the activation and conflict policy before writing data. Prefer a staged destination that leaves an existing desktop profile intact. Preserve account IDs and ownership, and make repeat import and collisions explicit.
- Take a consistent SQLite snapshot with the SQLite backup API or an equivalent SQLite-supported mechanism that includes committed WAL data. Open the source read-only and leave its database, WAL, and upload bytes untouched. Handle locking, busy retries, cancellation, and unsupported schema versions with actionable errors.
- Validate schema version, database integrity, foreign keys, and asset references. Use a staged database and asset directory, copy files with safe path handling, and coordinate publication so a failed or interrupted import leaves the previous usable destination intact. Preserve user IDs, bcrypt hashes, game and feature rows, archives, stable IDs, JSON fields, and image references. Require desktop sign-in with the imported account’s credentials. Report invalid, missing, or skipped data explicitly. Document the policy for unrecoverable input and a safe retry procedure.
- Test a disposable Go-created database with committed rows still in an active WAL, multiple accounts, archived entities, feature data, and referenced images. Verify the imported data in normal desktop flows after reopening. Test missing or corrupt files, unsupported schema, destination conflicts, cancellation, cleanup, and retry. Verify the source remains usable and unchanged. Keep web and desktop running against their respective live data stores.

## Phase 2. Game metadata, schema editing, formulas, and migration

### 4. Game metadata, object types, and fields

- Add game personal notes and cover editing to create and edit flows. Preserve saved notes and covers across subsequent edits. Match the web field limits and validation.
- Add type and field edit, archive, and restore actions. Keep IDs, saved values, references, placements, and archive flags stable. Match which active views exclude archived types or fields and which operations reject active dependencies. Wire the web’s type image fallback, field descriptions, field options, and primary display field selection and preview. An archived or incompatible primary display field must fall back safely.
- Support the full web field vocabulary and applicable defaults. Read effective defaults when a stored value is absent, including in views, filters, formulas, and goal evaluation where the web applies them. Distinguish absent values from `false`, zero, and structured values. Retain same-game and allowed-type constraints and the web’s treatment of existing links to archived targets.
- Port safe option edits. For select changes, validate and apply replacement mappings to affected values, defaults, and goal targets, including archived objects. For narrowed reference restrictions, require the same explicit authorization for removing incompatible links and apply the same dependency checks. Keep these operations atomic.

### 5. Formula fields

- Port the Go arithmetic evaluator and schema validation into native domain or service code. Preserve the expression grammar, stable field-ID references, unary operators, precedence, numeric conversion, dependency validation, and current parser and traversal limits. Resolve formulas from effective object data and expose errors for missing input, invalid or non-finite output, division by zero, and cycles as the web does.
- Add a native formula builder and unsaved preview using the same authoritative evaluator as saved objects. Include field insertion, dependency feedback, and the web’s relationship aggregation modes, including the `@{related}` binding and allowed linked target fields. Match handling of archived, missing, and incompatible related values.
- Use one native evaluator for object display, queries, goals, and preset effective values where applicable. Keep formula values derived and keep expression references stable across renames. Test shared vectors for valid arithmetic, missing values, precedence, cycles, aggregation, and bounds.

### 6. Safe field-type migration wizard

- Port preview and apply from `field_migration.go` and `field_migration_values.go`. Show what will change, impacted records, dependencies, conversion policies, and blockers. Include active and archived values, defaults, relationships, formulas, goals, section settings, primary displays, presets, and every dependency checked by the current web path.
- Use the same allowed conversions and explicit choices. Cover exact integer and decimal conversion, percentage clamping, progress current/maximum/percentage strategies, formula freezing, and reference serialization or mapping. Preserve the precision rules of the Go path and reject ambiguous inputs.
- Bind apply to the reviewed state. Revalidate inside the transaction and require a new preview if relevant data changed. Preserve field and object IDs and write the existing migration audit and per-object history records atomically with the conversion. Keep all state consistent on error. Test stale previews, blocked dependencies, exact conversions, archived records, and rollback on an injected failure.

## Phase 3. Sections, goals, object browsing, and history

### 7. Section editing, placement, and progress views

- Add editing, sibling ordering, and reparenting of existing sections. Provide discoverable native move controls and keyboard access. Match cycle prevention, stable IDs, and ordering rules. Preserve shared placements and the treatment of active children of archived parents.
- Implement Cards, List, Table, Checklist, and Progress using the same section configuration JSON. Respect ordered visible fields, primary display behavior, and selected object types. Checklist changes must update the canonical object value and appear in all views after refresh.
- Implement progress from a single current/maximum field, sums of configured numeric fields with a field-derived or fixed maximum, and sums across linked children. Keep direct traversal as the default for the existing configuration. Recursive traversal follows the selected same-type relationship, deduplicates eligible leaves per root, and ignores parent values when children exist. Match missing and archived data behavior, partial warnings, cycle reporting, and bounds from `section_progress.go`.
- Verify a shared object across sections, section archive and restore, movement cycle rejection, all view modes, direct and recursive results, shared descendant deduplication, incomplete data, and a cycle with no misleading percentage.

### 8. Goals and section goal placement

- Keep manual, checklist, numeric, prerequisite, and legacy scalar field-target workflows working. Add a native **ALL/ANY** condition builder and evaluate the same `target_json` rule representation as Go. Use type-appropriate fields, operators, and input controls for numeric, progress, formula, text, select, date, and boolean conditions as supported by web.
- Match live completion and progress calculations from canonical object values, missing and archived sources, and prerequisite locks and cycle checks. Preserve goal IDs, task IDs, task completion, and creation timestamps during edits. Support adding, removing, and reordering checklist tasks with stable identities.
- Add, display, and remove goal placements in sections, including adding an existing game goal to more than one section. Removing a placement preserves the goal and its progress. Match same-game validation and the web behavior when deleting a goal or archiving a section.
- Replace load-all goal paging with a bounded repository query or batched evaluation path. Match available filters, stable sorts, totals, and paging after live progress and prerequisite evaluation. Verify multi-page results, ties, and edits to a goal displayed in multiple sections.

### 9. Object search, filters, sorting, and pagination

- Add web-equivalent search to the desktop Object Library and section views. Include name, type, and description search, type scope, typed custom-field operators, ALL/ANY conditions, stable sorting, total counts, pagination, and saved per-section query settings.
- Apply criteria before pagination and match the web’s behavior for defaults, missing values, formula errors, archived records, and ties. Bind SQL values and validate field IDs, operators, and sort expressions. Use SQL for compatible cases and batched domain evaluation where required. Hydrate only the data needed for the current page or bounded operation.
- Add searchable selectors to reference editing, existing-object placement, formula preview, and goal rules. Ensure matches beyond the first page can be selected, existing selections persist while searching, and a response for a previously selected account, game, section, or filter cannot replace the current results.
- Test an empty collection, exact page boundaries, large and mixed-type collections, missing values, saved views after restart, and multiple simultaneous filter or sort changes.

### 10. Object History

- Preserve the existing timeline, archived object access, stable field IDs, and old/new value semantics. Match the web’s event, field, and UTC date filtering.
- Load a separate bounded set of up to 100 matching events for the selected numeric chart field, using the same filter scope as the web chart. Changing the timeline page should leave the chart’s data scope intact. Use responsive loading, clear empty and error states, and retry. Keep long reads off the GUI thread.
- Test a chart spanning multiple timeline pages, date and field changes, archived objects, stable field labels after rename, and events from an applied field migration.

## Phase 4. Templates, presets, dashboards, and object tools

### 11. Game Templates

- Replace the placeholder with a native library. Support saving a game structure, editing metadata, duplicating, deleting, importing and exporting versioned JSON, and creating a game from a template from both the library and the new game flow.
- Match the web’s inclusion controls, validation, size and library limits, and rules for portable resources. Preserve the format and reference remapping for sections, types, fields, formula dependencies, relationships, display fields, and saved section settings. Use fresh IDs for an independent game and commit instantiation atomically. The portable structure excludes personal objects, goals, progress, private notes, and uploaded images as defined by the web format.
- Test web export to desktop import and desktop export to web import using disposable fixtures. Instantiate on each side and compare structure after normalizing remapped IDs. Verify invalid documents, missing references, and unsupported versions produce actionable errors.

### 12. Build Presets

- Add per-object create, edit, duplicate, and delete flows. Support empty presets and snapshots of current base values. Preserve stable preset IDs on edits and match limits and accepted value types.
- Merge overrides with canonical fields in a dedicated effective-value path. Recalculate formulas and relationship aggregates with the effective values and enforce same-game reference rules. Canonical values and goal progress must keep using base object data. Match the web’s migration blockers and archived-record behavior.
- Verify explicit false and zero overrides, reference overrides, formula output, invalid ownership, reload, and comparison using a preset.

### 13. Custom Dashboard

- Add a per-game dashboard editor for adding, editing, resizing, reordering, removing, resetting, canceling, and saving widgets. Support recent objects, object collections, goals, progress, statistics, custom tables, notes, and images. Match the default layout, widget semantics, versioned JSON, validation, and bounds in `dashboard.go` and the web dashboard editor.
- Read live game data for widgets in bounded requests, with appropriate loading, retry, empty, and unavailable-source states. Preserve saved widget references when a source is archived. Keep image references synchronized with the layout transaction.
- Verify draft cancellation, save and reload, each widget type, account isolation, archived sources, and image deletion protection.

### 14. Object Gallery, Comparison, and Relationship Graph

- **Gallery** supports adding and reusing images, captions, order, one optional primary image, and removal. Keep archived galleries readable and enforce active-object edit rules. Removing an entry releases its asset reference. Verify that any other active or archived references still protect the asset.
- **Comparison** supports 2–4 active objects of the same type, including base and preset configurations, with the same field selection, reference labels, formula values, numeric differences, and configurable higher/lower preference as web. Open it from the native object browser and retain valid selections across pages.
- **Relationship Graph** opens from an object and supports node selection, branch expansion and collapse, center, zoom, and pan. Port bounded traversal and preserve direction, relationship field IDs and labels, and incoming/outgoing semantics. Deduplicate nodes and show archived nodes without expanding them. Check the web limits, which were depth 4, 100 nodes, and 200 edges per request at the audit. Keep expansion responsive and scoped to the active game.
- Verify gallery references and primary uniqueness, comparison ownership and type validation, and graph cycles, direction, archives, expansion, and truncation.

## Data integrity and performance requirements

- Reuse the shared schema and existing payloads wherever possible. For required shared changes, append semantically equivalent migrations to both histories, register the native SQL in the runner and resources, and keep applied migrations immutable. Test a fresh database and an upgrade from a representative previous version. Verify stable IDs, archived records, relationships, constraints, and preservation of unknown data where the format supports it.
- Use transactions for writes spanning related rows. Ensure filesystem operations, repeated actions, concurrent changes, and partial failures leave recoverable state. Use a snapshot or revalidation when a decision spans multiple reads. Surface validation, stale state, and recovery information through the UI.
- For long operations, use worker-owned named Qt SQL connections created and used on their owning thread. Release queries and handles before removing connections. Keep UI updates on the GUI thread and guard against stale results. Exercise large collections with realistic field, formula, goal, relationship, and widget data.

## Verification and completion

Use disposable databases, profiles, uploads, and templates. Prefer equivalent inputs and expected results across Go and Qt. Extend Qt repository and UI regression tests for each changed workflow. Reuse web tests as behavioral references and add corresponding web regression coverage for shared changes. Exercise the native UI through creation or selection, validation, save, reload, and archive or error handling where applicable.

From the repository root, run the web build, Go tests, frontend tests on Node.js 22 or newer, and vet using the existing Make targets or their equivalent commands.

```text
make build
make test
make test-js
make vet
```

From `desktop/`, use `mingw32-make build` and `mingw32-make test` with the matching Qt and compiler kit. The `windows-dev` configure, build, and CTest presets are equivalent. Verify the Windows portable ZIP and an extracted runtime smoke check for dependency, asset decoding, or packaging changes. Record any build or packaging checks skipped and their exact blockers. Use in-process web tests for contract checks. Interactive web testing requires an explicit request under `AGENTS.md`.

Before marking the task complete, verify the following end-to-end scenarios.

1. An existing desktop profile keeps its data. A separate Go-generated account signs in to desktop after import and can access its own games, archived records, and images.
2. A user defines a type with defaults, references, and formulas, then creates and edits a canonical object shown in multiple sections. Section views, search, and live goals reflect the saved data.
3. A reviewed field conversion applies atomically and appears in history. A stale preview is rejected. The chart spans matching events across timeline pages.
4. A versioned template crosses both directions between web and desktop. An instantiated game has internally consistent fresh IDs and the expected reusable structure.
5. An object preset affects formula results and comparisons, while goals continue to reflect the base object. The gallery and relationship graph expose the expected assets and links.
6. A custom dashboard persists through restart. An image referenced by an archived record remains protected. An invalid or interrupted import leaves the previous desktop data usable.

Update `desktop/docs/FEATURE_PARITY.md`, `desktop/docs/DATA_IMPORT.md`, and affected desktop user or build instructions to reflect verified behavior. In the final report, separate **Web (Go/JavaScript)** and **Desktop (C++/Qt)** checks. Include **Shared compatibility** for migration and interchange evidence and **Remaining limitations** for skipped checks or incomplete workflows with specific continuation steps. A feature is complete when its normal UI path executes its rules, persists or derives the correct result, and passes its applicable checks.
