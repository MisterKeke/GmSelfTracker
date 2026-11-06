# Data import status

The import wizard requested for the desktop port is not implemented yet. The current app creates its own database at `QStandardPaths::AppLocalDataLocation` and runs the same inspected SQLite migrations 001–011. It does not select, copy, open, or modify a GameTracker website database or uploads directory.

## Planned import contract

The implementation must select a source SQLite file and uploads folder, take a SQLite-supported consistent snapshot including active WAL state, leave the source untouched, validate schema version and integrity, preserve user hashes and all feature rows, copy asset files safely, and verify foreign keys and asset references before completion. It must report skipped rows and rollback the target transaction on failure. Existing source files must never be deleted or overwritten.

The source migration SQL has been copied into Qt resources so schema application can be developed independently of the Go server. The native migration runner applies one migration at a time in a transaction and recognizes quoted text, comments, and trigger bodies rather than splitting naively at every semicolon. A dedicated importer, SQLite backup API, asset copy service, and compatibility acceptance fixtures are still outstanding.
