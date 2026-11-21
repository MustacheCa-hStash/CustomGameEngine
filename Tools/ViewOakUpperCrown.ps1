param(
    [ValidateSet('fork','crown','lit','sunward','edge','grove')] [string]$Pose = 'crown',
    [ValidateSet(1,4)] [int]$Trees = 1,
    [switch]$Control, [string]$Capture, [string]$Python
)
$ErrorActionPreference = 'Stop'
if (!$Python) {
    $bundled = Join-Path $env:USERPROFILE '.cache/codex-runtimes/codex-primary-runtime/dependencies/python/python.exe'
    $Python = if (Test-Path $bundled) { $bundled } else { 'python' }
}
$options = @('--pose', $Pose.ToLowerInvariant(), '--trees', $Trees)
if ($Control) { $options += '--control' }
if ($Capture) { $options += @('--capture', $Capture) }
& $Python -B "$PSScriptRoot/OakLightingStudy/upper_crown.py" @options
exit $LASTEXITCODE
