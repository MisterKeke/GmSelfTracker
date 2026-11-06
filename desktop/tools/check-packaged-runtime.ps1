param(
    [Parameter(Mandatory = $true)][string]$Executable,
    [Parameter(Mandatory = $true)][string]$ReportPath
)

$ErrorActionPreference = 'Stop'
$Executable = (Resolve-Path -LiteralPath $Executable).Path
$ReportPath = [IO.Path]::GetFullPath($ReportPath)
if (Test-Path -LiteralPath $ReportPath) { Remove-Item -LiteralPath $ReportPath -Force }
$variables = @('PATH', 'QT_PLUGIN_PATH', 'QT_QPA_PLATFORM_PLUGIN_PATH', 'QT_QPA_PLATFORM', 'QT_QPA_FONTDIR',
    'QML_IMPORT_PATH', 'QML2_IMPORT_PATH', 'QT_STYLE_OVERRIDE')
$original = @{}
foreach ($name in $variables) { $original[$name] = [Environment]::GetEnvironmentVariable($name, 'Process') }
try {
    foreach ($name in $variables) { [Environment]::SetEnvironmentVariable($name, $null, 'Process') }
    $env:PATH = "$env:SystemRoot\System32;$env:SystemRoot"
    $env:QT_QPA_PLATFORM = 'offscreen'
    $arguments = '--check-runtime-report="{0}" --check-runtime' -f $ReportPath
    $runtime = Start-Process -FilePath $Executable -ArgumentList $arguments `
        -WorkingDirectory (Split-Path -Parent $Executable) -WindowStyle Hidden -PassThru
    if (!$runtime.WaitForExit(30000)) {
        $runtime.Kill()
        throw 'Packaged runtime check timed out after 30 seconds.'
    }
    $diagnostics = if (Test-Path -LiteralPath $ReportPath) { Get-Content -LiteralPath $ReportPath -Raw -Encoding UTF8 } else { 'No runtime diagnostic report was written.' }
    if ($runtime.ExitCode -ne 0) { throw "Packaged runtime check failed (exit $($runtime.ExitCode)):`n$diagnostics" }
    if ($diagnostics -notmatch 'PASS: packaged runtime') { throw "Packaged runtime check did not confirm success:`n$diagnostics" }
    Write-Host $diagnostics
} finally {
    foreach ($name in $variables) { [Environment]::SetEnvironmentVariable($name, $original[$name], 'Process') }
}
