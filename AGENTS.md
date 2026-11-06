# GmSelfTracker agent instructions

## Mandatory Dual-Platform Feature Parity

> **GmSelfTracker is one product with two implementations. Every new user-facing feature, feature extension, modification of existing shared behavior, and applicable bug fix must be implemented for both Go/Web and C++20/Qt Desktop within the same task. Functional parity, data compatibility, and verification of both versions are mandatory.**

Treat unqualified product requests as dual-platform requests. For example, adding a field type, fixing the object editor, improving goals, implementing filtering, updating the dashboard, or changing progress calculations all require inspecting both implementations without asking the user to repeat the scope. Deliver the working behavior in both, update relevant tests for each, verify shared data compatibility, and report each platform's status before calling the task complete.

- **Platform-specific work.** Scope explicitly and genuinely platform-specific requests to the affected platform, such as Qt window management, Windows portable packaging, native shortcuts, browser CSS, browser session cookies, or a platform build failure. Check the other implementation for an equivalent issue, preserve shared behavior, and explain why the other platform needs no change.
- **Bugs in shared behavior.** Inspect both paths even if the report names one UI. For an incorrect goal percentage, compare Go and C++ against the intended calculation. Fix every affected path and verify any unaffected path against the same expectation. For a Qt minimize defect, check for shared behavior impact and keep the fix native.
- **Existing gaps and incomplete work.** Check the source before assuming either app already supports a workflow. If a missing prerequisite prevents delivery on one platform, implement the parts needed for the requested workflow where feasible. If either applicable implementation remains incomplete, report the feature as incomplete, name the missing behavior and blocker, and give precise continuation steps. Keep persisted changes compatible wherever possible.
- **Working behavior is the completion gate.** Exercise the normal user flow through validation, business rules, and persistence where applicable. Buttons, placeholder screens, empty services, unimplemented API routes, hardcoded or mocked results, schema tables, and TODOs do not count as delivered functionality.

## Repository map

| Area | Web | Desktop |
| --- | --- | --- |
| Entry and UI | `cmd/server/main.go` serves the embedded app from `web/`. Follow `web/js/api.js`, `web/js/workspace/`, `web/js/editors/`, `web/js/components/`, `web/js/views/`, and `web/css/app.css`. | `desktop/src/main.cpp` starts the native app. Follow `desktop/src/app/main_window.cpp`, `desktop/src/app/theme.cpp`, and `desktop/src/presentation/widgets/`. |
| Domain and data | `internal/httpapi/` contains REST handlers, validation, authorization, and database queries. `internal/formula/` contains the arithmetic evaluator. `internal/database/` opens and migrates SQLite. | `desktop/src/domain/` defines records. `desktop/src/infrastructure/database/` contains `GameRepository` and `MigrationRunner`. `desktop/src/infrastructure/assets/` contains image decoding. |
| Tests | `internal/database/*_test.go`, `internal/httpapi/*_test.go`, `internal/formula/*_test.go`, and `web/js/tests/*.test.mjs`. | `desktop/tests/database_tests.cpp` and `desktop/tests/ui_tests.cpp`, registered with CTest in `desktop/CMakeLists.txt`. |

Use the source and tests to verify behavior. See [web architecture](docs/architecture.md), [desktop architecture](desktop/docs/ARCHITECTURE.md), and [desktop parity notes](desktop/docs/FEATURE_PARITY.md) for orientation. Treat status notes as snapshots and reconcile them with the current code.

## Workflow for shared changes

1. **Trace both implementations before editing.** Find the entry points, domain rules, validation, queries, schema and migrations, and existing tests. Identify differences in behavior and missing prerequisites from actual code.
2. **Plan both sides.** For substantial work, outline the web and desktop changes, shared data and migration impact, compatibility risks, and test coverage. Keep small fixes proportionate.
3. **Implement the shared behavior.** Use equivalent validation, calculations, ownership, limits, defaults, handling of missing values, and error conditions. Preserve behavior for existing records and callers outside the requested change.
4. **Wire both interfaces.** Use the existing Go handlers, SQLite access, REST API, vanilla ES modules, and CSS for web. Use C++20 domain logic, native SQLite access, and Qt Widgets, models, delegates, and dialogs for desktop. Use platform-appropriate interactions and keep the same operations discoverable. Keep desktop independent of Qt WebEngine, the Go server, and other shared runtime dependencies.
5. **Test both paths.** Run the relevant Go, frontend, and Qt tests. Add or update regression coverage for changed rules on both platforms and use equivalent inputs and expected outcomes where practical. Verify builds when the toolchains are available.
6. **Verify the data contract.** For persisted changes, verify fresh and upgraded disposable databases, preserved records, and equivalent interpretation of shared values and supported interchange formats. Explicitly record the checks you ran and any compatibility limit.
7. **Report per platform.** State what works, what was verified, and what remains, using the reporting format below.

Equivalent experience includes operations, validation, calculations, persistence, error behavior, supported field types, search, filtering, sorting, pagination, archive and restore, dependencies, and import and export formats. Native controls and internal designs may differ.

## Product and ownership rules

GmSelfTracker is a user-configurable game tracker. Users define games, nested sections, object types, fields, objects, goals, formulas, relationships, and dashboards. Derive behavior from configured types, fields, and relationships. Avoid hardcoded game mechanics or assumptions such as a character having a Level field, a weapon having Attack, or a location having exploration progress.

- **Objects belong to games.** Sections are views that reference canonical game-level objects through placements. An object can appear in multiple sections. An edit must be reflected in every relevant view. Removing a placement only removes the association. Preserve the canonical object when removing a placement or archiving a section.
- **Keep ownership and scope intact.** Respect the account-to-game boundary and same-game or same-type restrictions for placements, values, references, and goal dependencies. Enforce ownership and validation in authoritative handlers or native repository/domain code.
- **Preserve stable IDs.** Keep IDs for games, sections, object types, fields, objects, goals, presets, and other persisted entities during ordinary edits. Allocate and remap IDs deliberately for workflows that create independent copies, such as template instantiation, and rewrite their references consistently.

## Feature inventory and shared behavior

Use this inventory as a set of parity targets. Verify the relevant web and desktop paths before claiming either supports a feature. The desktop parity notes document gaps such as a local profile in place of account authentication and an unported formula evaluator. An embedded schema or a screen label gives no assurance that the user workflow exists.

| Feature area | Behavior to keep aligned |
| --- | --- |
| Authentication, game library, nested sections, and section placements | Preserve account and game isolation, shared placements, and Cards, List, Table, Checklist, and Progress view semantics. Keep browser authentication mechanics scoped to the web. |
| User-defined object types and custom fields | Match types, options, defaults, validation, and archive/restore. The web field vocabulary is `text`, `long_text`, `integer`, `decimal`, `percentage`, `boolean` (checkbox), `select`, `date`, `progress` (current/maximum), `reference`, `references`, and `formula`. Check native support in source. |
| Safe field-type migration | Match dry-run and apply behavior, compatible conversions, and dependency checks. Include archived values and defaults. Reject incompatible conversions consistently, and apply accepted changes atomically with the existing history and audit semantics. |
| Formula editor and evaluator | Match arithmetic, stable field ID references, dependency validation, circular-dependency detection, relationship aggregation, missing-value behavior, and errors. Preserve the expression language and its existing evaluation limits. |
| Object creation and editing, reference relationships, and Object History | Preserve canonical values, same-game links, history meaning, and stable field references. Respect archived records and existing view behavior. |
| Manual, Checklist, Numeric, and Field-target goals | Match completion, progress, numeric targets, ALL/ANY conditions, checklist behavior, and prerequisite locks. Validate dependency cycles and preserve shared goal placement where supported. |
| Recursive progress | Match traversal, eligible leaves, deduplication, cycle handling, archived objects, missing data, partial progress, and bounds. Preserve the distinction between direct and recursive configurations. |
| Game Templates and Build Presets | Keep supported template formats and ID remapping consistent. Evaluate preset overrides against the canonical object without replacing base values or changing goal evaluation. |
| Custom Dashboard, Object Gallery, Object Comparison, and Relationship Graph | Match relevant configuration, data selection, relationship semantics, image references, and bounds. |
| Image management and archive/restore | Keep ownership, references, validation, quotas, and cleanup consistent. Protect assets referenced by archived records as well as active records. Preserve archived values and links when restoring. |
| Search, filtering, sorting, and pagination | Match scope, field semantics, ordering, page boundaries, and totals. Verify large collections as well as empty and small results. |

## SQLite and data compatibility

The web app defaults to `data/gametracker.db` and `data/uploads/`. Desktop uses `gametracker.sqlite` under Qt's `QStandardPaths::AppLocalDataLocation`. Keep these live data stores separate. A common schema does not authorize either app to open the other's live database or allow simultaneous writes. Full web database and upload import into desktop is currently unavailable. See [desktop data import status](desktop/docs/DATA_IMPORT.md) for the planned contract. Preserve compatibility for supported interchange formats, such as the web's game template JSON, and verify both readers and writers when the format is supported on both sides.

For any shared persistence change:

1. Inspect `internal/database/migrations/` and `desktop/resources/migrations/`, their runners, and recorded `schema_migrations` versions. Check the actual history before choosing the next versions. Go orders the embedded SQL files by filename. Desktop uses an explicit ordered list in `desktop/src/infrastructure/database/migration_runner.cpp` and embeds files through `desktop/resources/resources.qrc`. Both runners record versions by position.
2. Append new versioned migrations in the correct order. Keep applied migrations immutable and preserve existing version positions. Register new native migrations in both the runner and Qt resources. Apply semantically equivalent shared changes in both apps, including column definitions, constraints, foreign keys, indexes, triggers, and default and backfill behavior.
3. Preserve existing rows, IDs, unknown fields, archived values, and reference links. Apply changes transactionally and retain integrity checks. Never delete or reset a real database, bypass migration validation, or silently discard user data. Use disposable databases and upload directories for tests.
4. Add migration coverage on both sides for fresh creation and upgrade from a representative previous schema. Verify rows and references survive, constraints remain enforced, and both apps interpret representative persisted values consistently. Test supported exchange formats with disposable fixtures. Report a missing import/export route as a compatibility limit and distinguish schema checks from verified round trips.

## Visual consistency

Preserve the shared colorful retro pixel-art style. Keep the purple and indigo backgrounds, cream panels, existing palette, pixel borders and typography, stepped shadows, decorative scenery, object card layout, progress bars, button hierarchy, and dialog styling. Use `web/css/app.css` and existing web components, plus `desktop/src/app/theme.cpp`, Qt style sheets (QSS), existing widgets, and custom painting. See [desktop visual parity notes](desktop/docs/VISUAL_PARITY.md).

Apply shared visual changes to both interfaces. Use native controls where appropriate, preserve recognizable hierarchy and styling, and allow platform-specific geometry. Keep platform-specific styling work confined to the element in scope. Preserve vanilla JavaScript and native Qt Widgets.

## Build and test

Keep the build systems independent. Run these web commands from the repository root. The Make targets invoke the corresponding Go and Node commands shown in `Makefile`. Frontend tests use Node.js 22 or newer.

```text
make build
make test
make test-js
make vet
```

Run the desktop commands from `desktop/` using the Windows Qt kit and its matching C++20 compiler. `desktop/Makefile` delegates to `desktop/tools/make.ps1`. Use `mingw32-make portable` for packaging work. It requires the vcpkg/libwebp setup described in [desktop build instructions](desktop/docs/BUILDING.md). Preserve the existing DevHub-inspired CMake, Make, and PowerShell workflow and the Something-Qt-inspired Windows deployment conventions.

```powershell
Set-Location desktop
mingw32-make build
mingw32-make test
mingw32-make portable
```

Alternatively, from `desktop/`, use the existing developer presets with `QT_ROOT_DIR` set and the matching compiler on `PATH`.

```powershell
cmake --preset windows-dev
cmake --build --preset windows-dev-debug
ctest --preset windows-dev-debug
```

For shared behavior, run the relevant Go, frontend, and Qt suites, verify both builds where possible, and check the data contract. Use disposable fixtures for databases, uploads, and profiles. Prefer source inspection and automated tests, including in-process HTTP tests. Launch the Go site only when interactive web testing is explicitly requested. Report which commands actually ran and their outcomes, and identify any checks skipped because tools or dependencies were unavailable.

## Code quality and regressions

- **Go.** Follow package conventions and `gofmt` changed Go files. Keep handlers focused, reuse domain and validation logic, bind SQL parameters, and preserve error handling, authorization, and transaction boundaries.
- **C++20 and Qt.** Follow existing class organization. Use RAII and explicit Qt object ownership. Keep domain logic separate from widgets, avoid unnecessary global state, and keep long operations off the GUI thread. Give workers their own named Qt SQL connections created and used on the owning thread, and release queries and handles before removing a connection. Preserve CMake and Qt Creator compatibility.
- **JavaScript.** Use vanilla ES modules and existing utilities and components. Preserve request cancellation or stale-result handling where applicable, and keep the server authoritative for validation and authorization.
- **All changes.** Keep edits scoped. Avoid unrelated refactors, unnecessary dependencies, duplicate rules within one implementation, unused placeholders, hardcoded machine paths, and silent data loss. For regression coverage, consider existing data, archives, other accounts and games, shared placements, field conversions, formula and goal dependencies, large collections, and consistent UI behavior.

## Completion checklist

- [ ] Traced the affected web and desktop workflows, data, and tests.
- [ ] Delivered working behavior in both apps, or verified and documented the reason for a platform-specific exception.
- [ ] Matched business rules and verified compatible persisted data and supported formats where applicable.
- [ ] Added or updated relevant regression coverage for both implementations and recorded the tests and builds actually run.
- [ ] Documented platform differences, verification limits, and remaining work. Checked for unfinished placeholders and reported any incomplete parity explicitly.

## Final implementation report

For shared changes, report **Web (Go/JavaScript)** and **Desktop (C++/Qt)** separately, with implementation status and executed checks for each. Under **Shared compatibility**, summarize behavior, schema and format changes, and compatibility evidence. Under **Remaining limitations**, identify skipped checks, platform-specific exceptions and their rationale, and any unfinished work with precise next steps. Claim completion only when all applicable implementations work.
