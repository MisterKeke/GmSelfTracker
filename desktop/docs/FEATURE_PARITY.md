# Feature parity status

Status reviewed against the source on 2026-11-07. **Stage 1 is implemented and verified for the native desktop application. Stage 2 was not started in this task.** The Web implementation remains the shared-behavior reference; the desktop application uses its own C++20, Qt Widgets, and SQLite runtime.

| Feature area | Web (Go/JavaScript) | Desktop (C++/Qt) |
|---|---|---|
| Runtime | Go HTTP server, SQLite, browser ES modules | Standalone Qt Widgets and SQLite; no WebEngine or Go runtime |
| Authentication | Account registration/login and browser session handling | Persistent local accounts, bcrypt registration/login, lock and switch, password change, last username only in QSettings, and access scoped to the signed-in account |
| Legacy desktop profile | Not applicable | The original local profile is retained. A one-time explicit “Secure Existing Local Profile” action adds credentials while preserving its ID and owned records. |
| Web data import | Source application | Imports a read-only, consistent schema-v11 database snapshot plus uploads. Preserves account IDs/hashes and stored records; see [data import](DATA_IMPORT.md). |
| Object types | Create, edit, archive/restore and configure fields | Create and edit name, description, icon, fallback image, primary display field; archive and restore while preserving objects and stable IDs |
| Custom fields | Supported web field vocabulary and validation | Create/edit supported field definitions, options, defaults, order, reference constraints, and formula expressions/aggregation; archive and restore with dependency checks |
| Select option edits | Replacement-based value updates | Add/rename/remove choices with explicit replacement mapping for saved values and defaults; updates are transactional |
| Field type migration | Web preview/apply and audit behavior | Native preview/apply flow with conversions, defaults, active and archived values, dependencies, representative examples, stale-preview detection, transaction rollback, audit rows, and Object History |
| Formula evaluation | Formula language and relationship aggregation | Stable field-ID references, arithmetic, formula dependency checks, aggregation, preview, and error reporting; formula-to-numeric conversion freezes current results only after explicit confirmation |
| Objects and relationships | Canonical game-owned objects and placements | Native object CRUD, same-game relationships, shared placements, archived values, and object history |
| Goals | Goal types, field targets, dependencies and rules | Manual, checklist, numeric, and field-target goals with same-game prerequisites and cycle prevention. Advanced configurable rules and section goal placement remain outside Stage 1. |
| Sections and views | Nested sections and five view modes | Nested sections, placements, browsing, archive and restore. Reordering/moving and the full configurable display modes remain Stage 2 work. |
| Templates, presets, dashboards and gallery | Supported web workflows | Data tables are retained by import, but native template, preset, dashboard, and gallery authoring workflows remain incomplete. |
| Search and collection navigation | Web behaviors | Game and goal search, goal filters/sort/paging, section object browsing, and history filters/paging. Full object filtering and repository-level pagination remain incomplete. |
| Image storage | Web upload lifecycle | Desktop-owned image library with ownership, quota/path validation, archive-aware references and cleanup. Import carries uploaded assets and their references. |
| Visual style | Retro pixel-art web UI | Native Qt controls follow the purple/indigo and cream palette, pixel borders, and stepped shadows. |

## Stage 1 implementation and verification

- Native registration, authentication, account isolation, password change, local lock/switch, and legacy-profile securing use bcrypt. Go-created `$2a$10$` hashes authenticate in the desktop app; a native-created hash was accepted by Go's bcrypt verifier.
- The importer requires a contiguous Web migration history through version 11 and its required tables. It reads through a read-only SQLite transaction, including active WAL state, checks integrity and foreign keys, stages assets with safe filenames and SHA-256 checks, and commits database rows only after validation. Conflicts stop the import without replacing existing records.
- Type and field edits preserve IDs. Archive/restore preserves objects, values, and links. Primary display fields must be active fields on their own type. Select removals require replacements for values and defaults.
- Field migration tests cover supported conversion families, exact integer JSON preservation, archived values and formula results, reference relationship handling, dependency blockers, preview staleness, and rollback.
- `cmake --build --preset windows-dev-debug --parallel 4` succeeded. `ctest --preset windows-dev-debug --output-on-failure` passed both native Qt test targets. `go test ./...` passed. `node --test web/js/tests/*.test.mjs` passed 24 tests.

## Remaining desktop parity work

Stage 1 is complete. Continue with Stage 2 only when requested. Later stages still need the remaining section views, complete search/filter/sort/pagination, recursive progress, template and preset workflows, configurable dashboards, gallery/comparison/relationship graph screens, and the broader source-equivalent acceptance matrix. No claim is made that the whole desktop product has feature parity yet.
