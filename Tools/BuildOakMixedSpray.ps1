param(
    [switch]$View, [switch]$Crown, [switch]$Benchmark,
    [ValidateSet('mixed','tri','coarse')] [string]$Arm = 'mixed',
    [ValidateRange(50,100)] [int]$Distance = 100,
    [string]$Python, [string]$Unreal = 'C:/Program Files/Epic Games/UE_5.8'
)
$ErrorActionPreference = 'Stop'
$build = Join-Path (Resolve-Path "$PSScriptRoot/..").Path 'build'
$cmake = (Select-String '^CMAKE_COMMAND:INTERNAL=(.*)' "$build/CMakeCache.txt").Matches.Groups[1].Value
& $cmake --build $build --config Release --target CustomGameEngine BakeOakBoughVoxels --parallel 6
if ($LASTEXITCODE) { exit $LASTEXITCODE }
if (!$Python) {
    $bundled = Join-Path $env:USERPROFILE '.cache/codex-runtimes/codex-primary-runtime/dependencies/python/python.exe'
    $Python = if (Test-Path $bundled) { $bundled } else { 'python' }
}
if ($Benchmark) {
    & $Python -B "$PSScriptRoot/MixedOakSpray.py" benchmark --unreal $Unreal
    if ($LASTEXITCODE -or !$View) { exit $LASTEXITCODE }
}
$action = if ($View) { 'view' } else { 'stage' }
$extra = if ($Crown) { @('--crown') } else { @() }
& $Python -B "$PSScriptRoot/MixedOakSpray.py" $action --arm $Arm --distance $Distance @extra
exit $LASTEXITCODE
