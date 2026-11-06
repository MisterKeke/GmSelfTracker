# Feature parity checklist

The current source tree was inspected statically. Its migrations directory contains 11 SQL files, through `011_object_gallery.sql`. The source checkout's `main` ref resolves to `0fb841bae13ea4390ed8a2e29b2bf8e9470d99ac`, matching the reference commit named in the supplied specification. The inspected DevHub and Something-Qt refs resolve to `c896d417a016f9b54689dbe702c5a17fb4b72678` and `cfee44d4cdb476b50be1f2a8b97d1800d8367a4d` respectively.

“Started” means there is a native foundation or data representation, not a complete source-equivalent feature.

| Area | Status | Current native behavior |
|---|---|---|
| C++20 / Qt 6 Widgets / CMake | Implemented | Native executable, CMake presets, Qt Creator-compatible project |
| No web runtime | Implemented | No WebEngine, server, HTTP API, or Go dependency |
| Retro theme and shell | Partial | Palette, pixel-style borders, sky strip, topbar, game rail, hero, tabs, section rail, empty states |
| Game library | Partial | Local create/edit, search, select, active-game counts, archive from the editor, and restore from Profile & Settings; import/export absent |
| Sections | Partial | Create root/nested sections, browse each section's placed objects, add or remove a shared object from active sections, archive from the tree context menu, and restore from settings. Section reordering, moving, goal placement, and the five configurable display modes remain |
| SQLite source schema | Partial | Source migrations 001–011 embedded and applied transactionally; existing user database import/backup is absent |
| Account/authentication | Not implemented | A local profile row is created; passwords, bcrypt compatibility, login, sessions, and password change are absent |
| Profile archive recovery | Partial | Profile & Settings lists and restores archived games, sections, and objects; uploaded-image management is absent |
| Object types and fields | Partial | Create types and add basic fields: text, long text, integer, decimal, percentage, boolean, select, date, progress, single/multiple reference. Edit, archive, formulas, defaults, and type-specific presentation settings are absent |
| Field migration wizard | Not implemented | Source migration SQL is present only |
| Object CRUD and relationships | Partial | Create, edit, archive, restore, persist supported field values, and link objects in the same game. Image editing, presets, and rich gallery are absent |
| Formula engine | Not implemented | No formula evaluation yet |
| Goals and prerequisites | Partial | Create, edit, and delete four goal types, including basic numeric field targets. Same-game prerequisites have cycle checks and completion gates. Goals support title and description search, status and type filters, sorting, and 20 goals per page. Advanced rule-builder targets and section placement remain |
| Recursive progress | Not implemented | No traversal service yet |
| Object history and charts | Partial | Active and archived objects have a native timeline with old and new values, stable field IDs, event/field/UTC date filters, and 20 events per page. The chart plots numeric changes on the displayed page. The timeline can display recorded field-type migrations, but the desktop migration wizard and background history loading remain |
| Templates and presets | Not implemented | Source table definitions are present only |
| Custom dashboard widgets | Partial | Overview shows recently edited objects and game statistics; widget configuration, persistence, and the other dashboard widgets are absent |
| Gallery, comparison, graph | Not implemented | No native feature screens yet |
| Image asset lifecycle | Partial | Byte/dimension validation and WebP decoder adapter; storage, quotas, references, and UI absent |
| Image formats | Partial | Qt formats plus libwebp adapter; portable build requires libwebp and runtime validation |
| Search, filters, sorting, pagination | Partial | Game name search, goal search/filter/sort and paging, history filters and paging, and object browsing by section. The goals page filters and sorts the complete loaded collection before building the visible cards. Repository-level object and goal paging remain |
| Automated tests | Partial | Disposable Qt Test suites cover migration parsing, object and goal persistence, section placement, history pagination and filters, and native UI navigation and goal filtering. The broader source-equivalent matrix remains |
| Portable packaging | Verified on this machine | The Release Make target produced a Windows x64 ZIP and SHA-256 file. An extracted copy passed the runtime check and rendered a native Windows preview with a system-only PATH. A separate clean-machine run remains |

No feature is marked complete solely because its source database table exists.

## Next implementation steps

1. Implement bcrypt-backed account selection and consistent SQLite and asset import together, so imported rows remain owned by their original account. Exercise the importer with a disposable active-WAL fixture.
2. Port the source field validation, formula evaluation, and direct and recursive progress services before expanding rule-based goals and section display modes.
3. Build the field migration, template, preset, dashboard, gallery, comparison, and relationship graph workflows against those services. Add worker-owned SQLite connections and verify large collections before final visual and clean-machine acceptance.
