param(
    [ValidateSet('rim','under','grove','reverse','review','far')] [string]$Pose = 'rim',
    [switch]$Baseline, [string]$Capture, [switch]$Rebuild, [string]$Python
)
$ErrorActionPreference = 'Stop'
if (!$Python) {
    $bundled = Join-Path $env:USERPROFILE '.cache/codex-runtimes/codex-primary-runtime/dependencies/python/python.exe'
    $Python = if (Test-Path $bundled) { $bundled } else { 'python' }
}
$options = @('--pose', $Pose.ToLowerInvariant())
if ($Baseline) { $options += '--baseline' }
if ($Capture) { $options += @('--capture', $Capture) }
if ($Rebuild) { $options += '--rebuild' }
& $Python -B "$PSScriptRoot/OakLightingStudy/run.py" @options
exit $LASTEXITCODE
