param([switch]$Refit, [switch]$CaptureNative, [switch]$Benchmark, [string]$Python,
      [string]$Unreal='C:/Program Files/Epic Games/UE_5.8')
$ErrorActionPreference='Stop'
$repo=(Resolve-Path "$PSScriptRoot/..").Path
$build=Join-Path $repo 'build'
$out=Join-Path $build 'oak-bough-voxel'
$project=Join-Path $build 'unreal-nanite-oak/FoliageCreation.uproject'
$null=New-Item -ItemType Directory -Path $out -Force
$cmake=(Select-String '^CMAKE_COMMAND:INTERNAL=(.*)' "$build/CMakeCache.txt").Matches.Groups[1].Value
if (!$Python) {
    $bundled=Join-Path $env:USERPROFILE '.cache/codex-runtimes/codex-primary-runtime/dependencies/python/python.exe'
    $Python=if(Test-Path $bundled){$bundled}else{'python'}
}
function Run-Python([string]$Script, [string[]]$Options) {
    & $Python -B "$PSScriptRoot/$Script.py" @Options
    if ($LASTEXITCODE) { throw "$Script exited $LASTEXITCODE" }
}
function Invoke-EditorFixture([string]$Script, [string]$Marker) {
    $log=Join-Path $out "$Script-$PID.log"
    $editorArgs=@("`"$project`"",'-EnablePlugins=PythonScriptPlugin,NaniteAssemblyEditorUtils,GLTFExporter',
        "-ExecutePythonScript=`"$PSScriptRoot/$Script.py`"",'-unattended','-RenderOffscreen','-NoSplash','-NoSound',
        '-NoP4','-NoCrashDialog','-noraytracing','-DDC=NoShared',"-abslog=`"$log`"")
    $proc=Start-Process -FilePath "$Unreal/Engine/Binaries/Win64/UnrealEditor-Cmd.exe" -ArgumentList $editorArgs -WindowStyle Hidden -Wait -PassThru
    if ($proc.ExitCode -ne 0 -or !(Select-String -Quiet -LiteralPath $log -Pattern $Marker)) {throw "See $log"}
}
& $cmake -S $repo -B $build
if ($LASTEXITCODE) {exit $LASTEXITCODE}
& $cmake --build $build --config Release --target BakeOakBoughVoxels CustomGameEngine --parallel 6
if ($LASTEXITCODE) {exit $LASTEXITCODE}
if ($CaptureNative) {
    $old=@($env:OAK_REUSABLE_BOUGH,$env:OAK_VOXEL_CONTROL,$env:OAK_CUT_OUTPUT)
    try {
        $env:OAK_REUSABLE_BOUGH='7';$env:OAK_VOXEL_CONTROL='1';$env:OAK_CUT_OUTPUT=$out
        Invoke-EditorFixture 'MakeOakJointControl' 'ASSEMBLIES_COMPLETE'
        Invoke-EditorFixture 'CaptureOakBoughVoxels' 'CAPTURES_COMPLETE'
        Invoke-EditorFixture 'CaptureOakBoughDepth' 'CAPTURES_COMPLETE'
        Invoke-EditorFixture 'CaptureOakBoughHoldouts' 'CAPTURES_COMPLETE'
    } finally {$env:OAK_REUSABLE_BOUGH,$env:OAK_VOXEL_CONTROL,$env:OAK_CUT_OUTPUT=$old}
}
if ($CaptureNative -or !(Test-Path "$out/native.oakvx")) {Run-Python DecodeOakBoughVoxels}
if ($Refit -or !(Test-Path "$out/partial-58-joint.oakvx")) {
    foreach ($mm in 19,29,38,58) {Run-Python FitOakBoughVoxels @('--mm',"$mm")}
    Run-Python FitOakBoughVoxels @('--group','part','--mm','29')
    Run-Python FitOakBoughVoxels @('--group','partial')
}
if ($Benchmark) {
    Run-Python BenchmarkOakBoughVoxels @('screen')
    Run-Python BenchmarkOakBoughVoxels @('finish')
    Run-Python ValidateOakBoughVoxels
    Run-Python SummarizeOakBoughVoxels
}
Run-Python BenchmarkOakBoughVoxels @('review')
