param(
    [Parameter(Mandatory = $true)]
    [ValidateSet('help', 'build', 'test', 'portable')]
    [string]$Task
)

$ErrorActionPreference = 'Stop'
$desktopRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$buildDirectory = Join-Path $desktopRoot 'out/build/windows-dev'

function Get-CMakeExecutable([string]$QtRoot) {
    $candidate = $env:CMAKE_PATH
    if ($candidate) {
        if (Test-Path -LiteralPath $candidate -PathType Container) { $candidate = Join-Path $candidate 'cmake.exe' }
        if (Test-Path -LiteralPath $candidate -PathType Leaf) { return (Resolve-Path -LiteralPath $candidate).Path }
        throw "CMAKE_PATH does not point to a CMake executable: $candidate"
    }
    $command = Get-Command 'cmake.exe' -ErrorAction SilentlyContinue
    if ($command) { return $command.Source }
    $command = Get-Command 'cmake' -ErrorAction SilentlyContinue
    if ($command) { return $command.Source }
    if ($QtRoot) {
        $qtDirectory = Get-Item -LiteralPath $QtRoot
        $toolRoot = Join-Path $qtDirectory.Parent.Parent.FullName 'Tools'
        $candidates = @(Get-ChildItem -Path (Join-Path $toolRoot 'CMake*\bin\cmake.exe') -ErrorAction SilentlyContinue)
        if ($candidates.Count -eq 1) { return $candidates[0].FullName }
        if ($candidates.Count -gt 1) { throw 'More than one Qt CMake bundle is installed. Set CMAKE_PATH to the intended executable.' }
    }
    throw 'CMake 3.28 or newer was not found. Add it to PATH or set CMAKE_PATH.'
}

function Get-CachedValue([string]$CachePath, [string]$Name) {
    if (!(Test-Path -LiteralPath $CachePath)) { return $null }
    $line = Get-Content -LiteralPath $CachePath | Where-Object { $_ -match "^$([regex]::Escape($Name)):[^=]*=" } | Select-Object -First 1
    if ($line) { return ($line -split '=', 2)[1] }
    return $null
}

function Get-QtRoot([string]$CachePath) {
    if ($env:QT_ROOT_DIR -and (Test-Path -LiteralPath $env:QT_ROOT_DIR -PathType Container)) {
        return (Resolve-Path -LiteralPath $env:QT_ROOT_DIR).Path
    }
    $cached = Get-CachedValue $CachePath 'CMAKE_PREFIX_PATH'
    if ($cached) {
        $first = ($cached -split ';')[0]
        if (Test-Path -LiteralPath $first -PathType Container) { return (Resolve-Path -LiteralPath $first).Path }
    }
    $qtpaths = Get-Command 'qtpaths6.exe' -ErrorAction SilentlyContinue
    if (!$qtpaths) { $qtpaths = Get-Command 'qtpaths.exe' -ErrorAction SilentlyContinue }
    if ($qtpaths) {
        $detected = & $qtpaths.Source --query QT_INSTALL_PREFIX
        if ($LASTEXITCODE -eq 0 -and (Test-Path -LiteralPath $detected -PathType Container)) { return (Resolve-Path -LiteralPath $detected).Path }
    }
    throw 'A Qt 6 kit was not found. Set QT_ROOT_DIR to its kit directory (for example, the directory containing bin/ and plugins/).'
}

function Get-CompilerBin([string]$QtRoot, [string]$CachePath) {
    $candidate = $env:COMPILER_BIN
    if ($candidate) {
        if (!(Test-Path -LiteralPath $candidate -PathType Container)) { throw "COMPILER_BIN is not a directory: $candidate" }
        $candidate = (Resolve-Path -LiteralPath $candidate).Path
    } else {
        $cachedCompiler = Get-CachedValue $CachePath 'CMAKE_CXX_COMPILER'
        if ($cachedCompiler -and (Test-Path -LiteralPath $cachedCompiler -PathType Leaf)) {
            $candidate = Split-Path -Parent (Resolve-Path -LiteralPath $cachedCompiler).Path
        }
    }
    if (!$candidate) {
        $qtDirectory = Get-Item -LiteralPath $QtRoot
        $qtInstallRoot = $qtDirectory.Parent.Parent.FullName
        $toolRoot = Join-Path $qtInstallRoot 'Tools'
        $compilerFiles = @(Get-ChildItem -Path (Join-Path $toolRoot 'mingw*\bin\g++.exe') -ErrorAction SilentlyContinue)
        if ($compilerFiles.Count -eq 1) { $candidate = $compilerFiles[0].DirectoryName }
        elseif ($compilerFiles.Count -gt 1) { throw 'More than one MinGW compiler is installed. Set COMPILER_BIN to the toolchain paired with this Qt kit.' }
    }
    if (!$candidate) { throw 'The Qt kit compiler could not be identified. Set COMPILER_BIN explicitly; an arbitrary MinGW compiler will not be selected.' }
    if (!(Test-Path -LiteralPath (Join-Path $candidate 'g++.exe')) -or !(Test-Path -LiteralPath (Join-Path $candidate 'gcc.exe'))) {
        throw "COMPILER_BIN must contain both g++.exe and gcc.exe: $candidate"
    }
    return (Resolve-Path -LiteralPath $candidate).Path
}

function Get-NinjaDirectory([string]$QtRoot) {
    $command = Get-Command 'ninja.exe' -ErrorAction SilentlyContinue
    if ($command) { return (Split-Path -Parent $command.Source) }
    $qtDirectory = Get-Item -LiteralPath $QtRoot
    $candidate = Join-Path $qtDirectory.Parent.Parent.FullName 'Tools/Ninja/ninja.exe'
    if (Test-Path -LiteralPath $candidate -PathType Leaf) { return (Split-Path -Parent $candidate) }
    throw 'Ninja was not found. Add it to PATH or install the Ninja tool distributed with Qt.'
}

function Invoke-CMake([string]$Executable, [string[]]$Arguments) {
    & $Executable @Arguments
    if ($LASTEXITCODE -ne 0) { throw "CMake command failed with exit code $LASTEXITCODE." }
}

if ($Task -eq 'help') {
    @'
GmSelfTracker Qt desktop commands
  build     Configure and build the Debug desktop app.
  test      Configure, build and run the disposable CTest suite.
  portable  Build Release, deploy Qt, run clean-runtime checks, create ZIP and SHA-256.
  help      Print this command list.

Configuration variables:
  QT_ROOT_DIR       Qt kit root. Required on a fresh machine.
  COMPILER_BIN      Paired MinGW bin directory; required if it cannot be identified uniquely.
  CMAKE_PATH        CMake executable or directory.
  JOBS              Parallel build workers (default 4, range 1–64).
  ZIP_NAME          Portable archive filename; filename only, ending in .zip.
  VCPKG_ROOT        vcpkg checkout; required for libwebp and portable packaging.
  VCPKG_INSTALLED_DIR  Optional package install directory.

Output: out/build/windows-dev, out/build/windows-package, out/portable-stage, out/release.
'@ | Write-Host
    exit 0
}

$cachePath = Join-Path $buildDirectory 'CMakeCache.txt'
$qtRoot = Get-QtRoot $cachePath
$cmake = Get-CMakeExecutable $qtRoot
$compilerBin = Get-CompilerBin $qtRoot $cachePath
$ninjaDirectory = Get-NinjaDirectory $qtRoot
$parallelism = 4
if ($env:JOBS) {
    if ($env:JOBS -notmatch '^\d+$' -or [int]$env:JOBS -lt 1 -or [int]$env:JOBS -gt 64) { throw 'JOBS must be an integer from 1 to 64.' }
    $parallelism = [int]$env:JOBS
}

$env:QT_ROOT_DIR = $qtRoot
$env:PATH = "$compilerBin;$($qtRoot)\bin;$ninjaDirectory;$env:PATH"
$env:QT_PLUGIN_PATH = Join-Path $qtRoot 'plugins'
$env:QT_QPA_PLATFORM_PLUGIN_PATH = Join-Path $qtRoot 'plugins/platforms'
$env:CC = Join-Path $compilerBin 'gcc.exe'
$env:CXX = Join-Path $compilerBin 'g++.exe'

if ($Task -eq 'portable') {
    & (Join-Path $PSScriptRoot 'package-windows.ps1') -PortableOnly -QtRoot $qtRoot -CompilerBin $compilerBin -CMakePath $cmake -BuildParallelism $parallelism -ZipName $env:ZIP_NAME
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
    exit 0
}

$configureArgs = @('--preset', 'windows-dev', "-DCMAKE_PREFIX_PATH=$qtRoot")
if ($env:VCPKG_ROOT) {
    $vcpkgToolchain = Join-Path $env:VCPKG_ROOT 'scripts/buildsystems/vcpkg.cmake'
    if (!(Test-Path -LiteralPath $vcpkgToolchain)) { throw "VCPKG_ROOT has no CMake toolchain: $vcpkgToolchain" }
    $configureArgs += "-DCMAKE_TOOLCHAIN_FILE=$vcpkgToolchain"
    $configureArgs += '-DVCPKG_TARGET_TRIPLET=x64-mingw-static'
    if ($env:VCPKG_INSTALLED_DIR) { $configureArgs += "-DVCPKG_INSTALLED_DIR=$env:VCPKG_INSTALLED_DIR" }
}
Invoke-CMake $cmake $configureArgs

if ($Task -eq 'build') {
    Invoke-CMake $cmake @('--build', '--preset', 'windows-dev-debug', '--parallel', "$parallelism")
    exit 0
}

Invoke-CMake $cmake @('--build', '--preset', 'windows-dev-debug', '--parallel', "$parallelism")
$ctest = Join-Path (Split-Path -Parent $cmake) 'ctest.exe'
if (!(Test-Path -LiteralPath $ctest -PathType Leaf)) { throw "CTest executable was not found beside CMake: $ctest" }
& $ctest --preset windows-dev-debug
if ($LASTEXITCODE -ne 0) { throw "CTest failed with exit code $LASTEXITCODE." }
exit 0
