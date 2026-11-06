# Windows portable deployment

Portable ZIP deployment is the release target. It packages the native Qt Widgets executable, Qt runtime DLLs, the Windows and offscreen platform plugins, the SQLite driver, Qt image-format plugins, compiler runtime files, license notices, and a runtime report.

## Package

Install vcpkg and ensure its `libwebp` port can be built with the MinGW toolchain paired with Qt. Then:

```powershell
$env:QT_ROOT_DIR = 'D:/Qt/6.11.0/mingw_64'
$env:VCPKG_ROOT = 'D:/Tools/vcpkg'
mingw32-make portable
```

The script configures Release, builds, stages with CMake install, invokes the Qt Widgets `windeployqt`, checks required files, validates the staged runtime with development plugin paths removed, writes the ZIP, and hashes it. It deploys `qoffscreen` because the clean runtime smoke check uses Qt's offscreen platform. `StageOnly` is available for the PowerShell script when only a verified stage is wanted. A missing plugin, runtime dependency, license notice, or image decoder fails the task.

Expected outputs:

- `out/portable-stage/bin/GmSelfTrackerQt.exe`
- `out/release/GmSelfTrackerQt-Portable-0.1.0-x64.zip`
- `out/release/SHA256SUMS.txt`

The database remains under Qt's per-user application-data path and does not live beside the executable. Extract and run the complete package directory; do not copy out only the executable.

The 0.1.0 package was built with Qt 6.11.0 and MinGW GCC 13.1.0. After verifying its ZIP checksum, the extracted package passed the runtime smoke check and rendered a disposable Goals preview using the Windows platform plugin and a system-only PATH. These checks ran on the build machine.

## WebP

The inspected Qt kit does not contain a `qwebp` image plugin. The desktop uses the vcpkg `libwebp` decoder directly, and the portable build requires that library and exercises a disposable encode/decode probe. It does not silently claim WebP support based on a different development installation.

## Runtime validation and licensing

`check-packaged-runtime.ps1` clears Qt and compiler development paths before launch. The application checks QSQLITE, applies its migrations to a temporary database, writes/reads temporary game and section rows, verifies PNG/JPEG/GIF/WebP/BMP decoding, and instantiates the representative Widgets shell. It does not open user data or access a service.

The packaging script copies the Qt distribution's license directory and libwebp's vcpkg copyright notice. Review those notices against the license option selected for redistribution before publishing a release.
