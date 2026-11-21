param(
    [switch]$View, [switch]$Benchmark,
    [ValidateSet('compute','tri','gpu-tri','gpu-qualified','box')] [string]$Arm = 'compute',
    [ValidateRange(50,175)] [int]$Distance = 100,
    [string]$Python
)
$ErrorActionPreference = 'Stop'
$build = Join-Path (Resolve-Path "$PSScriptRoot/..").Path 'build'
$cmake = (Select-String '^CMAKE_COMMAND:INTERNAL=(.*)' "$build/CMakeCache.txt").Matches.Groups[1].Value
& $cmake --build $build --config Release --target CustomGameEngine --parallel 6
if ($LASTEXITCODE) { exit $LASTEXITCODE }
if (!$Python) {
    $bundled = Join-Path $env:USERPROFILE '.cache/codex-runtimes/codex-primary-runtime/dependencies/python/python.exe'
    $Python = if (Test-Path $bundled) { $bundled } else { 'python' }
}
if ($Benchmark) {
    & $Python -B "$PSScriptRoot/OakGroveBricks.py" benchmark
    if ($LASTEXITCODE -or !$View) { exit $LASTEXITCODE }
}
$action = if ($View) { 'view' } else { 'stage' }
& $Python -B "$PSScriptRoot/OakGroveBricks.py" $action --arm $Arm --distance $Distance
exit $LASTEXITCODE
