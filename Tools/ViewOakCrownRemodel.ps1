param(
    [ValidateSet('fork','reverse','three-quarter','grove','rim','backlight')] [string]$Pose = 'three-quarter',
    [ValidateSet(1,4)] [int]$Trees = 1,
    [ValidateRange(0,5)] [int]$Revision = 5,
    [string]$Capture, [string]$Python
)
$ErrorActionPreference = 'Stop'
if (!$Python) {
    $bundled = Join-Path $env:USERPROFILE '.cache/codex-runtimes/codex-primary-runtime/dependencies/python/python.exe'
    $Python = if (Test-Path $bundled) { $bundled } else { 'python' }
}
$options = @('--pose', $Pose.ToLowerInvariant(), '--trees', $Trees, '--revision', $Revision)
if ($Capture) { $options += @('--capture', $Capture) }
& $Python -B "$PSScriptRoot/OakLightingStudy/remodel.py" @options
exit $LASTEXITCODE
