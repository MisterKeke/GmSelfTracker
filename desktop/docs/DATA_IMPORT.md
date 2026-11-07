# Web data import

The desktop app provides **Settings → Import Existing Web Data**. It imports accounts and game records into the desktop database at `QStandardPaths::AppLocalDataLocation`; the Web database and uploads remain separate live stores. A successful import does not sign in to an imported account. Use **Lock / Switch Account** and sign in with that account's existing Web username and password.

## Supported source

- A GmSelfTracker SQLite database with contiguous schema migrations 1–11 and all required version-11 tables, including `asset_references`.
- The matching original uploads directory. Active assets must be UUID-named PNG files within the Web upload size limit and match their recorded sizes. Shared asset references are preserved.
- Browser `sessions` and `schema_migrations` rows are not copied. Desktop runs its own migrations; users authenticate again locally.

The importer opens SQLite read-only with `query_only`, starts a read transaction, and validates the database inside that snapshot. This includes committed rows in an active WAL file. It checks `integrity_check`, the migration sequence, required tables, `foreign_key_check`, and asset references before copying any target rows. The source database and source upload files are never modified.

## Import and rollback behavior

The desktop app first stages and verifies each required asset in a temporary directory. It then copies the source tables inside one target SQLite transaction, restores primary-display references after field rows exist, recreates the gallery validation triggers, copies asset-reference metadata, checks foreign keys, and installs assets using atomic file writes. If validation or a write fails, the target transaction is rolled back and files installed during that attempt are removed. If the process stops after an asset file is installed but before the database commit, the database remains unchanged; a retry accepts an identical existing asset and can continue.

Imported rows retain their original IDs, account hashes, archived state, object history, formulas, relationships, placements, goals and dependencies, templates, presets, dashboards, gallery metadata, and asset IDs. The copied tables are `uploaded_assets`, `users`, `games`, `sections`, `object_types`, `field_definitions`, `objects`, `field_values`, `object_relationships`, `section_placements`, `goals`, `goal_tasks`, `section_goal_placements`, `object_history_events`, `object_history_changes`, `field_type_migrations`, `game_templates`, `object_presets`, `goal_dependencies`, `game_dashboards`, and `object_gallery`; `asset_references` is copied with its conflict checks.

No merge or ID remapping is attempted. An account ID or case-insensitive username collision stops the import before target changes. Any later row or reference collision rolls back the transaction. Existing assets are reused only when their size and SHA-256 digest match; conflicting files or metadata are rejected and never overwritten. A repeated import of the same nonempty source stops on its existing accounts. Resolve the collision in the selected inputs or target and retry; the importer will not silently combine distinct records.

## Verification and limits

Qt database tests create a disposable version-11 source using the embedded source migrations, keep uncheckpointed rows in an active WAL, and verify accounts, multiple games, archived records, references, history, templates, presets, dashboards, gallery rows, primary display configuration, and a shared asset. A second test removes a required asset and confirms the attempt rolls back without adding the source account. These are source-compatible fixture checks; no real user database or uploads directory was imported during verification.

The source importer currently accepts schema version 11 only. Upgrade the Web database to version 11 before importing. Browser sessions are intentionally not transferable. The import tests exercise validation failures and transactional rollback, but do not kill the process at every possible interruption point or emulate every filesystem permission failure. Desktop and Web live databases remain separate; do not point both apps at the same live database for concurrent writes.
