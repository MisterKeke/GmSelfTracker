# GameTracker

GameTracker is a local-first, customizable game progress tracker. You create the games, sections, object types, fields, and goals that make sense to you. The app does not ship game-specific rules or preset characteristics.

The visual design uses an indigo and plum game interface, parchment panels, stepped borders, pixel-style progress bars, and responsive layouts inspired by the supplied retro RPG dashboard.

## Stack

- Go HTTP server and JSON REST API
- SQLite through `database/sql` and the pure-Go `modernc.org/sqlite` driver
- Embedded HTML, CSS, and Vanilla JavaScript modules
- GNU Make for local commands

The Go server serves the frontend and `/api` from one listener. The app does not need Node.js, npm, Docker, a separate frontend server, or a cloud service.

## Run locally

Requirements: Go 1.25 or newer and GNU Make. On Windows, if Make is installed as `mingw32-make`, run that command with the same target.

```text
make dev
```

Open [http://localhost:8080](http://localhost:8080). The first run creates the SQLite database and applies the embedded, versioned migrations. Register with a username and password to start a library.

Useful commands:

```text
make build
make test
make fmt
make vet
make help
```

On the Windows environment used during implementation, GNU Make is available as:

```powershell
mingw32-make dev
```

## Local data and configuration

Default files:

- SQLite database: `data/gametracker.db`
- Uploaded images: `data/uploads/`

The database and uploads are ignored by Git. Set environment variables before `make dev` to override defaults:

| Variable | Default | Purpose |
| --- | --- | --- |
| `HOST` | `127.0.0.1` | Listener address |
| `PORT` | `8080` | Listener port |
| `DB_PATH` | `./data/gametracker.db` | SQLite file path |
| `UPLOAD_DIR` | `./data/uploads` | Local image directory |
| `APP_ENV` | `development` | Set to `production` to mark the session cookie Secure |

`.env.example` documents these values. The app reads environment variables directly and does not require a `.env` file.

## Core concepts

- **Game:** the owner-scoped data boundary.
- **Section:** a custom, nested view that organizes object references. Sections support cards, lists, tables with selectable fields, checklists backed by a user-defined checkbox field, and progress views backed by a user-defined current/maximum field.
- **Object type:** a user-defined schema such as “Character”, “Location”, or “Project”. These names have no special backend meaning.
- **Field definition:** a typed characteristic attached to an object type. Its type is immutable after creation. Fields can be archived and restored without losing values.
- **Object:** a game-level record with one object type. Objects are not copied when placed into sections.
- **Field value:** data for one object and one stable field ID. Ordinary values are validated by Go and stored as JSON in a relational table. Object references use a separate relational table and are restricted to the same game.
- **Goal:** a game-level manual goal, checklist, numeric tracker, or live target linked to a numeric object field.

See [docs/architecture.md](docs/architecture.md) for the schema and ownership boundaries.

### Supported field types

Text, long text, integer, decimal, percentage, checkbox, select, date, current/maximum progress, single object reference, multiple object references, and calculated formula.

Calculated formulas are read-only and use a small arithmetic grammar. Reference fields use stable IDs such as `@{a-stable-field-id}`. The evaluator accepts numbers, parentheses, unary signs, addition, subtraction, multiplication, and division. It does not execute JavaScript or user code. Formula references must point to numeric fields on the same object type; cross-object aggregation is not implemented.

## Authentication and privacy

Passwords use bcrypt. Authenticated sessions use random opaque IDs stored as hashes in SQLite and HttpOnly, SameSite cookies. State-changing API requests require a CSRF token. Private queries check the current user through the game owner. Login attempts are rate-limited per client IP in the running process.

This is a local development application. Production deployment still needs an HTTPS reverse proxy, backups, operational monitoring, and a deployment-specific security review.

## Images

Game covers and object images are stored locally. Uploads accept valid PNG, JPEG, and GIF images up to 5 MB, with dimension limits. User-provided images are used in place of bundled game artwork.

## Example workflow

1. Register, then add a game.
2. Open **Types & Fields** and create an object type.
3. Add the fields you want. Choose field icons from the built-in symbol suggestions or enter another emoji/symbol.
4. Create objects with dynamic forms generated from the type's fields.
5. Add nested sections and place the same object in multiple sections.
6. Create manual goals, checklist goals, numeric progress goals, or field targets.
7. Refresh or restart the app. Records remain in the SQLite file.

## API overview

The API uses JSON and authenticated cookies. Main route groups:

- `/api/auth/*` — register, login, logout, current user, password change
- `/api/games/*` — game library and game-scoped sections, object types, objects, and goals
- `/api/sections/*/placements` — shared many-to-many object placement
- `/api/object-types/*/fields` and `/api/fields/*` — custom field definitions and archive/restore
- `/api/objects/*/values` — validated values and references
- `/api/goals/*` and `/api/goal-tasks/*` — goal progress and checklist tasks
- `/api/assets/*` — local image uploads and authenticated image reads

Errors use a JSON `error` object containing a stable code and readable message. Collections support pagination where they can grow most quickly (games and objects).

## Current limits

- No email, password reset, cloud sync, game-service imports, or multi-user sharing.
- Formula calculations work on numeric fields of the same object. Relationship aggregation is not implemented.
- Goal targets currently support numeric values; date, text, and multi-condition goal rules are not implemented.
- Calculated progress from multiple fields and parent progress from child objects are not implemented; progress views use an explicitly selected current/maximum field.
- Custom image uploads for field icons, object type icons, section images, and game icons are not yet available. Game cover and object image uploads work.
- Object types cannot yet be archived. Games, sections, objects, and fields have archive/restore paths.
- Goals are managed in the game workspace and are not yet placeable into custom sections.
- Uploads accept PNG, JPEG, and GIF only.

## Tests

Run:

```text
make test
```

Tests use temporary SQLite databases and in-process HTTP handlers. They cover migrations, authentication, ownership isolation, CSRF, shared placements, field validation and lifecycle, formulas, relationships, linked goal progress, and image validation.
