param(
    [ValidateSet('current','soil','layered','optimized')] [string]$Arm = 'optimized',
    [ValidateSet('edge','roots','blade','sun','layout')] [string]$Pose = 'edge',
    [switch]$Capture, [switch]$Prepare, [string]$Python
)
$ErrorActionPreference = 'Stop'
if (!$Python) {
    $bundled = Join-Path $env:USERPROFILE '.cache/codex-runtimes/codex-primary-runtime/dependencies/python/python.exe'
    $Python = if (Test-Path $bundled) { $bundled } else { 'python' }
}
$builder = Join-Path $PSScriptRoot 'MeadowGrassStudy/coverage.py'
$manifest = Join-Path $PSScriptRoot "../build/meadow-coverage/$Arm/scene/metadata.json"
if ($Prepare -or !(Test-Path $manifest)) {
    & $Python -B $builder prepare --arm $Arm
    if ($LASTEXITCODE) { exit $LASTEXITCODE }
}
$mode = if ($Capture) { 'capture' } else { 'view' }
& $Python -B $builder $mode --arm $Arm --pose $Pose
exit $LASTEXITCODE
