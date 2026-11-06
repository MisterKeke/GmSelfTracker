# Architecture

The native target is divided into these layers:

```text
Qt Widgets presentation
        ↓
Application window and interaction handlers
        ↓
Domain records and validation
        ↓
SQLite repository and migration runner
```

`MainWindow` owns navigation and display state. The separate object history dialog renders timeline events and a numeric chart using domain records from the repository. `GameRepository` handles game, section, placement, field, object, relationship, and history access through prepared QtSql statements and transactions. `MigrationRunner` initializes SQLite, applies the source migrations transactionally, and records applied versions. `ImageDecoder` validates source image byte and dimension limits and delegates WebP to the optional linked libwebp decoder.

The UI is native Qt Widgets. It uses layouts, a `QStackedWidget`, a `QTreeWidget` for the section outline, QSS for the shared palette, and `QPainter` for the sky strip. There is no HTML, JavaScript, WebEngine, local HTTP listener, or Go runtime dependency.

Database connections are owned by one repository instance and used on the GUI thread in this milestone. The repository is non-copyable and closes its named QtSql connection before removal. Background query workers, per-thread connections, import workers, and cancellation have not yet been added.

## Current persistence boundary

The app writes its own database in the Qt application-data location. It adds a local desktop profile row so game foreign keys can use the source schema. It does not import existing users, passwords, or game data. The sentinel password field is not an authentication credential; desktop authentication has not been implemented.

## Build and deployment references

The Windows build layout follows the inspected DevHub conventions: CMake Presets, Ninja Multi-Config, Make shortcuts, PowerShell orchestration, CTest, staged deployment, ZIP output, and SHA-256. Qt Widgets deployment follows Something-Qt's explicit `windeployqt` plugin checks and user-data separation, without importing its QML architecture or dependencies.
