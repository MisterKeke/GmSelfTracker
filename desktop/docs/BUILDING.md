# Building on Windows

## Toolchain

- Windows x64.
- CMake 3.28 or newer.
- Ninja Multi-Config.
- Qt 6 Widgets, Core, Gui, Sql, and Qt Test components.
- A Qt-compatible C++20 compiler. For a MinGW Qt kit, use the MinGW distribution paired with that kit.
- vcpkg and the `libwebp` port when building portable packages or enabling WebP support.

No absolute machine paths are stored in CMake files. Set kit and tool locations in the process environment or pass Make variables.

## Make shortcuts

From `desktop/`:

```powershell
$env:QT_ROOT_DIR = 'D:/Qt/6.11.0/mingw_64'
mingw32-make help
mingw32-make build
mingw32-make test
```

Path and parallelism overrides are forwarded as environment values, so spaces in `ZIP_NAME` do not become shell syntax:

```powershell
mingw32-make build QT_ROOT_DIR=D:/Qt/6.11.0/mingw_64
mingw32-make test QT_ROOT_DIR=D:/Qt/6.11.0/mingw_64 JOBS=4
mingw32-make portable ZIP_NAME=GmSelfTracker-Retro.zip
mingw32-make portable "ZIP_NAME=GmSelfTracker Retro.zip"
```

Set `COMPILER_BIN` if more than one MinGW is installed and kit pairing is ambiguous. Set `CMAKE_PATH` to choose a CMake executable or directory. `JOBS` accepts 1–64 and defaults to 4.

## CMake presets

With `QT_ROOT_DIR` set and the Qt-paired compiler on `PATH`:

```powershell
cmake --preset windows-dev
cmake --build --preset windows-dev-debug
ctest --preset windows-dev-debug
cmake --build --preset windows-dev-release
```

The developer preset uses Ninja Multi-Config and supports Debug, Release, and RelWithDebInfo. `windows-ci` is the optional Visual Studio 2022 x64 configuration when a matching MSVC Qt kit is installed. `windows-analysis` enables warnings as errors. The package preset builds optimized Release and disables tests.

When enabling the manifest dependency with CMake directly, set `VCPKG_ROOT` and configure with:

```powershell
cmake --preset windows-dev `
  -DCMAKE_TOOLCHAIN_FILE="$env:VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake" `
  -DVCPKG_TARGET_TRIPLET=x64-mingw-static
```

The Make wrapper detects the vcpkg toolchain when `VCPKG_ROOT` is set. Runtime/test data is disposable. The test target never opens the original site or persistent user database.

## Output directories

- `out/build/windows-dev`: development configurations.
- `out/build/windows-package`: optimized release build.
- `out/portable-stage`: staged executable and runtime.
- `out/release`: portable ZIP and SHA-256 file.
