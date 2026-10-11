param([switch]$WorkerRecovery)
$ErrorActionPreference = 'Stop'
$engine = Join-Path $PSScriptRoot 'desktop-control.exe'
if (-not (Test-Path -LiteralPath $engine)) { $engine = Join-Path $PSScriptRoot 'bin/desktop-control.exe' }
if (-not (Test-Path -LiteralPath $engine)) { throw 'desktop-control.exe was not found beside the diagnostic script or in bin/' }
& $engine --diagnostics
if ($LASTEXITCODE) { throw 'Diagnostic command failed' }
if ($WorkerRecovery) {
    & $engine --test-worker-recovery
    if ($LASTEXITCODE) { throw 'Worker recovery test failed' }
}
