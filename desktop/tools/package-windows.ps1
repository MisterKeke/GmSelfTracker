param(
    [switch]$PortableOnly,
    [switch]$StageOnly,
    [string]$QtRoot,
    [string]$CompilerBin,
    [string]$CMakePath,
    [ValidateRange(1, 64)][int]$BuildParallelism = 4,
    [string]$ZipName
)

$ErrorActionPreference = 'Stop'
$desktopRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$outputRoot = Join-Path $desktopRoot 'out'
$buildRoot = Join-Path $desktopRoot 'out/build/windows-package'
$stageDirectory = Join-Path $desktopRoot 'out/portable-stage'
$releaseDirectory = Join-Path $desktopRoot 'out/release'

if (!$QtRoot) { $QtRoot = $env:QT_ROOT_DIR }
if (!$CompilerBin) { $CompilerBin = $env:COMPILER_BIN }
if (!$CMakePath) { $CMakePath = $env:CMAKE_PATH }
if (!$QtRoot -or !(Test-Path -LiteralPath $QtRoot -PathType Container)) { throw 'QtRoot must point to the selected Qt 6 kit.' }
if (!$CompilerBin -or !(Test-Path -LiteralPath $CompilerBin -PathType Container)) { throw 'CompilerBin must point to the MinGW compiler paired with the selected Qt kit.' }
if (!$CMakePath) {
    $cmakeCommand = Get-Command 'cmake.exe' -ErrorAction SilentlyContinue
    if (!$cmakeCommand) { $cmakeCommand = Get-Command 'cmake' -ErrorAction SilentlyContinue }
    if (!$cmakeCommand) { throw 'CMake was not found. Set CMakePath or CMAKE_PATH.' }
    $CMakePath = $cmakeCommand.Source
}
if (Test-Path -LiteralPath $CMakePath -PathType Container) { $CMakePath = Join-Path $CMakePath 'cmake.exe' }
if (!(Test-Path -LiteralPath $CMakePath -PathType Leaf)) { throw "CMake executable not found: $CMakePath" }
if (!$env:VCPKG_ROOT -or !(Test-Path -LiteralPath (Join-Path $env:VCPKG_ROOT 'scripts/buildsystems/vcpkg.cmake'))) {
    throw 'Portable packaging requires vcpkg for the libwebp image decoder. Set VCPKG_ROOT to a vcpkg checkout.'
}

if (!$env:VCPKG_INSTALLED_DIR) { $env:VCPKG_INSTALLED_DIR = Join-Path $desktopRoot 'out/deps/vcpkg_installed' }
$env:QT_ROOT_DIR = (Resolve-Path -LiteralPath $QtRoot).Path
$env:COMPILER_BIN = (Resolve-Path -LiteralPath $CompilerBin).Path
$env:PATH = "$env:COMPILER_BIN;$env:QT_ROOT_DIR\bin;$env:PATH"
$env:CC = Join-Path $env:COMPILER_BIN 'gcc.exe'
$env:CXX = Join-Path $env:COMPILER_BIN 'g++.exe'
$env:QT_PLUGIN_PATH = Join-Path $env:QT_ROOT_DIR 'plugins'
$env:QT_QPA_PLATFORM_PLUGIN_PATH = Join-Path $env:QT_ROOT_DIR 'plugins/platforms'

if ($ZipName) {
    if ([IO.Path]::GetFileName($ZipName) -ne $ZipName -or $ZipName.IndexOfAny([IO.Path]::GetInvalidFileNameChars()) -ge 0 -or $ZipName -notmatch '\.zip$') {
        throw 'ZIP_NAME must be a .zip filename without a directory path.'
    }
}

$configure = @('--preset', 'windows-package', "-DCMAKE_PREFIX_PATH=$env:QT_ROOT_DIR",
    "-DCMAKE_TOOLCHAIN_FILE=$(Join-Path $env:VCPKG_ROOT 'scripts/buildsystems/vcpkg.cmake')",
    '-DVCPKG_TARGET_TRIPLET=x64-mingw-static', "-DVCPKG_INSTALLED_DIR=$env:VCPKG_INSTALLED_DIR")
& $CMakePath @configure
if ($LASTEXITCODE -ne 0) { throw "CMake package configure failed with exit code $LASTEXITCODE." }
& $CMakePath --build --preset windows-package-release --parallel $BuildParallelism
if ($LASTEXITCODE -ne 0) { throw "Release build failed with exit code $LASTEXITCODE." }

$resolvedStage = [IO.Path]::GetFullPath($stageDirectory)
if (!$resolvedStage.StartsWith($outputRoot + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase)) {
    throw "Refusing to clear a stage path outside desktop/out: $resolvedStage"
}
if (Test-Path -LiteralPath $resolvedStage) { Remove-Item -LiteralPath $resolvedStage -Recurse -Force }
New-Item -ItemType Directory -Path $resolvedStage -Force | Out-Null
& $CMakePath --install $buildRoot --config Release --prefix $resolvedStage
if ($LASTEXITCODE -ne 0) { throw "CMake install/staging failed with exit code $LASTEXITCODE." }

$executable = Join-Path $stageDirectory 'bin/GmSelfTrackerQt.exe'
if (!(Test-Path -LiteralPath $executable -PathType Leaf)) { throw "Staged application was not found: $executable" }
$deployTool = Join-Path $env:QT_ROOT_DIR 'bin/windeployqt.exe'
if (!(Test-Path -LiteralPath $deployTool -PathType Leaf)) { throw "Qt Widgets deployment tool is missing: $deployTool" }
$deployArgs = @('--release', '--compiler-runtime', '--no-translations', '--add-plugin-types', 'imageformats', '--include-plugins', 'qwindows,qoffscreen,qsqlite')
& $deployTool @deployArgs $executable
if ($LASTEXITCODE -ne 0) { throw "windeployqt failed with exit code $LASTEXITCODE." }

$requiredFiles = @(
    'bin/Qt6Core.dll', 'bin/Qt6Gui.dll', 'bin/Qt6Widgets.dll', 'bin/Qt6Sql.dll',
    'bin/GmSelfTrackerQt.exe', 'bin/platforms/qwindows.dll', 'bin/platforms/qoffscreen.dll', 'bin/sqldrivers/qsqlite.dll',
    'bin/imageformats/qgif.dll', 'bin/imageformats/qjpeg.dll'
)
foreach ($relative in $requiredFiles) {
    $path = Join-Path $stageDirectory $relative
    if (!(Test-Path -LiteralPath $path -PathType Leaf)) { throw "Required packaged runtime file is missing: $relative" }
}

$licenseDirectory = Join-Path $stageDirectory 'licenses'
New-Item -ItemType Directory -Path $licenseDirectory -Force | Out-Null
$qtNoticeDirectory = Join-Path $licenseDirectory 'Qt'
New-Item -ItemType Directory -Path $qtNoticeDirectory -Force | Out-Null
$qtInstallRoot = (Get-Item -LiteralPath $env:QT_ROOT_DIR).Parent.Parent.FullName
$qtLicenseSource = Join-Path $qtInstallRoot 'Licenses'
if (!(Test-Path -LiteralPath $qtLicenseSource -PathType Container)) { throw "Qt redistribution notices were not found: $qtLicenseSource" }
New-Item -ItemType Directory -Path $qtNoticeDirectory -Force | Out-Null
Copy-Item -Path (Join-Path $qtLicenseSource '*') -Destination $qtNoticeDirectory -Recurse -Force
$webpNotice = Join-Path $env:VCPKG_INSTALLED_DIR 'x64-mingw-static/share/libwebp/copyright'
if (!(Test-Path -LiteralPath $webpNotice -PathType Leaf)) { throw "libwebp license notice was not found: $webpNotice" }
Copy-Item -LiteralPath $webpNotice -Destination (Join-Path $licenseDirectory 'libwebp-LICENSE.txt') -Force

$report = Join-Path $stageDirectory 'runtime-check.txt'
& (Join-Path $PSScriptRoot 'check-packaged-runtime.ps1') -Executable $executable -ReportPath $report
if ($LASTEXITCODE -ne 0) { throw 'Packaged runtime validation failed.' }
if (!(Test-Path -LiteralPath $report -PathType Leaf) -or (Get-Content -LiteralPath $report -Raw) -notmatch 'PASS: packaged runtime') {
    throw 'Runtime smoke-check did not write its success marker.'
}

if ($StageOnly) {
    Write-Host "Portable runtime staged and validated: $stageDirectory"
    return
}

New-Item -ItemType Directory -Path $releaseDirectory -Force | Out-Null
$archiveName = if ($ZipName) { $ZipName } else { 'GmSelfTrackerQt-Portable-0.1.0-x64.zip' }
$archive = Join-Path $releaseDirectory $archiveName
Add-Type -AssemblyName System.IO.Compression.FileSystem
if (Test-Path -LiteralPath $archive) { Remove-Item -LiteralPath $archive -Force }
[IO.Compression.ZipFile]::CreateFromDirectory($stageDirectory, $archive, [IO.Compression.CompressionLevel]::Optimal, $false)
$sha256 = [Security.Cryptography.SHA256]::Create()
try {
    $archiveStream = [IO.File]::OpenRead($archive)
    try {
        $hash = [BitConverter]::ToString($sha256.ComputeHash($archiveStream)).Replace('-', '').ToLowerInvariant()
    } finally {
        $archiveStream.Dispose()
    }
} finally {
    $sha256.Dispose()
}
"$hash *$archiveName" | Set-Content -LiteralPath (Join-Path $releaseDirectory 'SHA256SUMS.txt') -Encoding ascii
Write-Host "Portable archive: $archive"
Write-Host "SHA-256: $hash"
