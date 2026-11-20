param(
    [ValidateSet('crown','7','8')] [string]$Subject = 'crown',
    [ValidateSet('sprays','hybrid','native','native-compact','current','production','tiered','canopy','canopy1','reusable-native-trunk')] [string]$Arm = 'sprays',
    [ValidateSet(41,100)] [int]$Distance = 100,
    [ValidateRange(0.001,16)] [double]$PixelError = 1,
    [switch]$View, [switch]$Repack, [switch]$Recapture, [switch]$Benchmark,
    [string]$Python, [string]$Unreal = 'C:/Program Files/Epic Games/UE_5.8'
)
$ErrorActionPreference = 'Stop'
if (!$PSBoundParameters.ContainsKey('Arm') -and $Subject -eq 'crown') { $Arm = 'tiered' }
if ($Arm -eq 'reusable-native-trunk') { $Arm = 'canopy1' }
if ($Subject -ne 'crown' -and $Arm -in @('production','canopy','canopy1')) { throw 'This selection requires the full crown.' }
$root = (Resolve-Path "$PSScriptRoot/..").Path
$build = Join-Path $root 'build'
$out = Join-Path $build 'oak-live-selection'
$cmake = (Select-String '^CMAKE_COMMAND:INTERNAL=(.*)' "$build/CMakeCache.txt").Matches.Groups[1].Value
& $cmake --build $build --config Release --target CustomGameEngine --parallel 6
if ($LASTEXITCODE) { exit $LASTEXITCODE }
if (!$Python) {
    $bundled = Join-Path $env:USERPROFILE '.cache/codex-runtimes/codex-primary-runtime/dependencies/python/python.exe'
    $Python = if (Test-Path $bundled) { $bundled } else { 'python' }
}
New-Item -ItemType Directory -Path $out -Force | Out-Null
if ($Recapture) {
    $project = Join-Path $build 'unreal-nanite-oak/FoliageCreation.uproject'
    & "$PSScriptRoot/BuildOakAssemblyBench.ps1" -Project $project -Unreal $Unreal
    if ($LASTEXITCODE) { exit $LASTEXITCODE }
    $log = Join-Path $out "native-$PID.log"
    $editorArgs = @("`"$project`"", '-EnablePlugins=PythonScriptPlugin,OakAssemblyBench',
        "-ExecutePythonScript=`"$PSScriptRoot/CaptureOakSelector.py`"", "-abslog=`"$log`"",
        '-unattended','-RenderOffscreen','-NoSound','-NoSplash','-NoP4','-NoCrashDialog','-DDC=NoShared')
    $oldNativeData = $env:OAK_LIVE_NATIVE_DATA
    $captureFolder = Join-Path $out ("resident-" + [guid]::NewGuid().ToString('N'))
    try {
        $env:OAK_LIVE_NATIVE_DATA = $captureFolder
        $p = Start-Process "$Unreal/Engine/Binaries/Win64/UnrealEditor-Cmd.exe" -ArgumentList $editorArgs -WindowStyle Hidden -Wait -PassThru
        if ($p.ExitCode -or !(Select-String -Quiet -LiteralPath $log -SimpleMatch 'OAK_CAPTURE CAPTURES_COMPLETE')) {
            throw "Native hierarchy capture failed. Check $log."
        }
        foreach ($s in '7','8','crown') {
            if (!(Test-Path "$captureFolder/dump-$s/Base/ResourceDescs.json") -or !(Test-Path "$captureFolder/layout-$s.txt")) {
                throw "Missing resident hierarchy data for $s. Check $log."
            }
        }
        Set-Content "$out/native-input.txt" -Value $captureFolder -Encoding UTF8
    } finally {
        $env:OAK_LIVE_NATIVE_DATA = $oldNativeData
    }
}
$packSubjects = if ($Benchmark -or $Recapture -or $Repack) { @('7','8','both','crown') } else { @($Subject) }
foreach ($s in $packSubjects) {
    if ($Repack -or $Recapture -or !(Test-Path "$out/native-$s.oakcut") -or !(Test-Path "$out/hybrid-$s.oakcut") -or !(Test-Path "$out/sprays-$s.oakcut")) {
        & $Python -B "$PSScriptRoot/PackOakLiveCuts.py" --subjects $s
        if ($LASTEXITCODE) { exit $LASTEXITCODE }
    }
}
if ($Arm -in @('tiered','canopy','canopy1') -and !$Benchmark -and
    ($Recapture -or $Repack -or !(Test-Path "$out/$Arm-$Subject.oakcut"))) {
    & $Python -B "$PSScriptRoot/PackOakLiveCuts.py" --finalist $Arm --subjects $Subject
    if ($LASTEXITCODE) { exit $LASTEXITCODE }
}
if ($Benchmark) {
    foreach ($action in 'calibrate','suite','hybrid','screen','guard','canopy','motion','counters','frame-time') {
        & $Python -B "$PSScriptRoot/BenchmarkOakLiveCuts.py" $action
        if ($LASTEXITCODE) { exit $LASTEXITCODE }
    }
    & $Python -B "$PSScriptRoot/ProfileOakLiveMaterials.py"
    if ($LASTEXITCODE) { exit $LASTEXITCODE }
    $pilotAssets = Join-Path $build 'oak-assembly-step/reusable/pilot/assets'
    & "$build/Release/BakeSprayBough.exe" --pack-static-order "$pilotAssets/WhiteOak-ForkedCanopy.glb" `
        "$pilotAssets/WhiteOak-ForkedCanopy.leaves.fixedlod" "$pilotAssets/WhiteOak-ForkedCanopy.spray-wood.fixedlod" "$out/cache-trunk.treelod"
    if ($LASTEXITCODE) { exit $LASTEXITCODE }
    & $Python -B "$PSScriptRoot/BenchmarkOakLiveCuts.py" order
    if ($LASTEXITCODE) { exit $LASTEXITCODE }
    & $Python -B "$PSScriptRoot/SummarizeOakLiveCuts.py"
    if ($LASTEXITCODE) { exit $LASTEXITCODE }
}
$action = if ($View) { 'view' } else { 'stage' }
$overrides = @()
if ($PSBoundParameters.ContainsKey('PixelError')) { $overrides = @('--pixel-error',$PixelError.ToString([CultureInfo]::InvariantCulture)) }
& $Python -B "$PSScriptRoot/BenchmarkOakLiveCuts.py" $action --subject $Subject --arm $Arm --distance $Distance @overrides
exit $LASTEXITCODE
