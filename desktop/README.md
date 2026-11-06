# GmSelfTracker Qt Desktop

This native C++20 and Qt 6 Widgets port uses its own SQLite database. See the [feature parity checklist](docs/FEATURE_PARITY.md) for current coverage. The [web version](../README.md#run-locally) has its own setup and build commands.

## Requirements

- CMake 3.28 or newer and a C++20 compiler.
- Qt 6.5 or newer with Core, Gui, Widgets, Sql, and the QSQLITE driver. Building tests also requires Qt Test.
- Ninja and the MinGW compiler paired with the Qt kit for the Windows developer and portable Make workflows.
- Visual Studio 2022 x64 and a matching MSVC Qt kit for the separate Windows CI preset.
- vcpkg with libwebp for the portable package.

## Build, test, and run

From the desktop directory, set `QT_ROOT_DIR` to the Qt kit and use the Make shortcuts:

```powershell
$env:QT_ROOT_DIR = 'C:/Qt/6.11.0/mingw_64'
mingw32-make help
mingw32-make build
mingw32-make test
```

Before running the Debug executable, put the paired MinGW bin directory and the Qt kit's bin directory on PATH. The Qt Creator kit can also launch the executable.

```powershell
.\out\build\windows-dev\Debug\GmSelfTrackerQt.exe
```

Set `COMPILER_BIN` if the Make wrapper cannot identify the paired compiler. See [build setup](docs/BUILDING.md) for CMake presets and additional toolchain settings.

## Portable ZIP

Set `VCPKG_ROOT` to your vcpkg checkout and run:

```powershell
mingw32-make portable
```

The ZIP and SHA-256 checksum are written to `out/release`. Extract the full ZIP and run `bin/GmSelfTrackerQt.exe`. See [Windows deployment](docs/WINDOWS_DEPLOYMENT.md) for the packaging and smoke test details.

## Local data and Go data import

The desktop stores `gametracker.sqlite` under Qt's `QStandardPaths::AppLocalDataLocation`. On a standard Windows profile this is `%LOCALAPPDATA%\GmSelfTracker\GmSelfTrackerQt`. The app applies its embedded SQLite migrations there. The web version defaults to `data/gametracker.db` at the repository root.

Existing Go database and uploads import is unavailable in this port. Keep the web database and its uploads together for continued use with the web version. See [data import status](docs/DATA_IMPORT.md).

## Runtime checks

The app supports `--check-runtime` for an isolated offscreen smoke check and `--render-preview=<path> --preview-size=1920x1080` for a disposable UI image. Add `--preview-page=goals` to render the goals screen with sample goals, or use `--preview-interactive=goals` to open that sample screen with the normal Windows font and widget backends. These modes use temporary database data.
