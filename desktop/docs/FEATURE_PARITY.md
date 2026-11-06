# Feature parity checklist

The current source tree was inspected statically. Its migrations directory contains 11 SQL files, through `011_object_gallery.sql`. The source checkout's `main` ref resolves to `0fb841bae13ea4390ed8a2e29b2bf8e9470d99ac`, matching the reference commit named in the supplied specification. The inspected DevHub and Something-Qt refs resolve to `c896d417a016f9b54689dbe702c5a17fb4b72678` and `cfee44d4cdb476b50be1f2a8b97d1800d8367a4d` respectively.

“Started” means there is a native foundation or data representation, not a complete source-equivalent feature.

| Area | Status | Current native behavior |
|---|---|---|
| C++20 / Qt 6 Widgets / CMake | Implemented | Native executable, CMake presets, Qt Creator-compatible project |
| No web runtime | Implemented | No WebEngine, server, HTTP API, or Go dependency |
| Retro theme and shell | Partial | Palette, pixel-style borders, sky strip, topbar, game rail, hero, tabs, section rail, empty states |
| Game library | Partial | Local create/edit, search, select, active-game counts, archive from the editor, and restore from Profile & Settings. Create/edit now preserve personal notes and choose a cover from the owned image library; game database import/export remains absent |
| Sections | Partial | Create root/nested sections, browse each section's placed objects, add or remove a shared object from active sections, archive from the tree context menu, and restore from settings. Section reordering, moving, goal placement, and the five configurable display modes remain |
| SQLite source schema | Partial | Source migrations 001–011 embedded and applied transactionally; existing user database import/backup is absent |
| Account/authentication | Not implemented | A local profile row is created; passwords, bcrypt compatibility, login, sessions, and password change are absent |
| Profile archive recovery | Partial | Profile & Settings lists and restores archived games, sections, and objects, and opens the local image manager. Type/field archives and imported accounts are not available |
| Object types and fields | Partial | Create types with fallback images and create fields with descriptions and validated typed defaults, including explicit `false` and zero. Effective defaults are returned for active fields. Type/field editing, archive/restore, option editing, primary display settings, and safe option migration remain |
| Field migration wizard | Not implemented | Source migration SQL is present only |
| Object CRUD and relationships | Partial | Create, edit, archive, restore, persist supported field values, and link objects in the same game. Objects can choose an owned image; cards show object art or the type fallback. Presets and rich gallery are absent |
| Formula engine | Partial | Create a formula field with stable field-ID references, arithmetic, relationship aggregation (sum/avg/min/max/count), a live preview against an example object, dependency evaluation, and per-object error display. Formula option editing, formula-backed goal rules, and preset effective values remain |
| Goals and prerequisites | Partial | Create, edit, and delete four goal types, including basic numeric field targets. Same-game prerequisites have cycle checks and completion gates. Goals support title and description search, status and type filters, sorting, and 20 goals per page. Advanced rule-builder targets and section placement remain |
| Recursive progress | Not implemented | No traversal service yet |
| Object history and charts | Partial | Active and archived objects have a native timeline with old and new values, stable field IDs, event/field/UTC date filters, and 20 events per page. The numeric chart now loads a separate bounded set of the latest 100 matching field changes with the same date range, independent of timeline paging. The timeline can display recorded field-type migrations, but the desktop migration wizard, chart retry state, and background history loading remain |
| Templates and presets | Not implemented | Source table definitions are present only |
| Custom dashboard widgets | Partial | Overview shows recently edited objects and game statistics; widget configuration, persistence, and the other dashboard widgets are absent |
| Gallery, comparison, graph | Not implemented | No native feature screens yet |
| Image asset lifecycle | Partial | Local profile storage normalizes decoded images to PNG, uses generated UUID names under a private uploads directory, checks resolved paths, enforces 5 MB/6000 px/16 MP, and accounts for the 1000-file/250 MB quota. Owned-library selection, game/type/object references, archived-record protection, queued deletion, bounded cleanup, retry, and save failure handling are wired and covered. Login/import and galleries are absent |
| Image formats | Partial | PNG, JPEG, GIF, and BMP are available in this build. WebP is available only when libwebp is linked; portable packaging requires libwebp |
| Search, filters, sorting, pagination | Partial | Game name search, goal search/filter/sort and paging, history filters and paging, and object browsing by section. The goals page filters and sorts the complete loaded collection before building the visible cards. Repository-level object and goal paging remain |
| Automated tests | Partial | Disposable Qt Test suites cover migration parsing, object and goal persistence, section placement, history pagination and filters, formula arithmetic/defaults/relationship aggregation/cycles, typed field defaults, and image validation, quota, ownership, archived references, failed cleanup/retry, and native image/default/formula editor flows. The broader source-equivalent matrix remains |
| Portable packaging | Verified on this machine | The Release Make target produced a Windows x64 ZIP and SHA-256 file. An extracted copy passed the runtime check and rendered a native Windows preview with a system-only PATH. A separate clean-machine run remains |

No feature is marked complete solely because its source database table exists.

## Next implementation steps

1. Implement bcrypt-backed account selection and consistent SQLite and asset import together, so imported rows remain owned by their original account. Exercise the importer with a disposable active-WAL fixture.
2. Finish type and field editing/archive recovery, dependency-aware option changes, formula option editing, and formula use in goals and presets. Then port direct and recursive section progress and rule-based goals.
3. Build the field migration, template, preset, dashboard, gallery, comparison, and relationship graph workflows against those services. Add worker-owned SQLite connections and verify large collections before final visual and clean-machine acceptance.
