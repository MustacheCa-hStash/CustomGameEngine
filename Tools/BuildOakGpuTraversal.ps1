param(
    [ValidateSet(1,4)] [int]$Trees = 4,
    [ValidateSet(24,41,100)] [int]$Distance = 41,
    [ValidateSet(132,23,197)] [int]$Yaw = 132,
    [ValidateRange(.001,16)] [double]$PixelError = 1,
    [ValidateSet('native-gpu','native-packed','native-cpu','canopy1-packed','canopy1-gpu','canopy1-cpu','tiered-gpu','tiered-cpu','production')]
    [string]$Arm = 'native-gpu',
    [switch]$View, [switch]$Benchmark, [switch]$Recapture, [switch]$RasterProfile,
    [string]$Python, [string]$Unreal = 'C:/Program Files/Epic Games/UE_5.8'
)
$ErrorActionPreference = 'Stop'
$root = (Resolve-Path "$PSScriptRoot/..").Path
$build = Join-Path $root 'build'
$cmake = (Select-String '^CMAKE_COMMAND:INTERNAL=(.*)' "$build/CMakeCache.txt").Matches.Groups[1].Value
& $cmake --build $build --config Release --target CustomGameEngine --parallel 6
if ($LASTEXITCODE) { exit $LASTEXITCODE }
if (!$Python) {
    $bundled = Join-Path $env:USERPROFILE '.cache/codex-runtimes/codex-primary-runtime/dependencies/python/python.exe'
    $Python = if (Test-Path $bundled) { $bundled } else { 'python' }
}
if ($Recapture) {
    $project = "$build/unreal-nanite-oak/FoliageCreation.uproject"
    & "$PSScriptRoot/BuildOakAssemblyBench.ps1" -Project $project -Unreal $Unreal
    if ($LASTEXITCODE) { exit $LASTEXITCODE }
    $log = "$build/oak-gpu-ue.log"
    $editorArgs = @("`"$project`"", '-EnablePlugins=PythonScriptPlugin,OakAssemblyBench',
        "-ExecutePythonScript=`"$PSScriptRoot/CaptureOakGpuComparison.py`"", "-abslog=`"$log`"",
        '-unattended','-RenderOffscreen','-NoSound','-NoSplash','-NoP4','-NoCrashDialog','-DDC=NoShared')
    $p = Start-Process "$Unreal/Engine/Binaries/Win64/UnrealEditor-Cmd.exe" -ArgumentList $editorArgs -WindowStyle Hidden -Wait -PassThru
    if ($p.ExitCode -or !(Select-String -LiteralPath $log -SimpleMatch 'OAK_CAPTURE CAPTURES_COMPLETE' -Quiet)) {
        throw "Native oak comparison failed. See $log."
    }
    $log = "$build/oak-gpu-game.log"
    $gameArgs = @("`"$project`"", '/Game/NaniteOakBench/Benchmark','-game','-OakGameTiming','-EnablePlugins=OakAssemblyBench',
        '-unattended','-RenderOffscreen','-NoSound','-NoSplash','-NoP4','-NoCrashDialog','-DDC=NoShared',
        '-windowed','-ForceRes','-ResX=3840','-ResY=2160','-NoVSync',"-abslog=`"$log`"")
    $p = Start-Process "$Unreal/Engine/Binaries/Win64/UnrealEditor-Cmd.exe" -ArgumentList $gameArgs -WindowStyle Hidden -Wait -PassThru
    if ($p.ExitCode -or !(Select-String -LiteralPath $log -SimpleMatch 'OakGame COMPLETE' -Quiet)) {
        throw "Standalone game timing failed. See $log."
    }
}
$families = if ($RasterProfile) { @('native') } elseif ($Benchmark) { @('native','tiered','canopy1') } else { @($Arm.Split('-')[0]) }
foreach ($family in $families) {
    if ($family -eq 'production' -or (Test-Path "$build/oak-live-selection/$family-crown.oakcut")) { continue }
    $extra = if ($family -eq 'native') { @() } else { @('--finalist',$family) }
    & $Python -B "$PSScriptRoot/PackOakLiveCuts.py" --subjects crown @extra
    if ($LASTEXITCODE) { exit $LASTEXITCODE }
}
if ($RasterProfile) {
    & "$PSScriptRoot/BuildOakAssemblyBench.ps1" -Unreal $Unreal
    if ($LASTEXITCODE) { exit $LASTEXITCODE }
    & $Python -B "$PSScriptRoot/ProfileOakRaster.py" all --unreal $Unreal
    exit $LASTEXITCODE
}
if ($Benchmark) {
    & $Python -B "$PSScriptRoot/BenchmarkOakGpuTraversal.py" suite
    if ($LASTEXITCODE) { exit $LASTEXITCODE }
}
$action = if ($View) { 'view' } else { 'stage' }
$extra = @()
if ($PSBoundParameters.ContainsKey('PixelError')) {
    $extra = @('--pixel-error', $PixelError.ToString([CultureInfo]::InvariantCulture))
}
& $Python -B "$PSScriptRoot/BenchmarkOakGpuTraversal.py" $action --trees $Trees --distance $Distance --yaw $Yaw --arm $Arm @extra
exit $LASTEXITCODE
