param([switch]$Rebake, [string]$Python, [string]$Unreal='C:/Program Files/Epic Games/UE_5.8')
$ErrorActionPreference='Stop'
$repo=(Resolve-Path "$PSScriptRoot/..").Path
$build=Join-Path $repo 'build'
$trial=Join-Path $build 'oak-assembly-step/reusable'
$assets=Join-Path $build 'Release/assets/WhiteOak-ForkedCanopy'
$cmake=(Select-String '^CMAKE_COMMAND:INTERNAL=(.*)' "$build/CMakeCache.txt").Matches.Groups[1].Value
& $cmake --build $build --config Release --target CustomGameEngine BakeSprayBough --parallel 6
if ($LASTEXITCODE) {exit $LASTEXITCODE}
if (!$Python) {
    $bundled=Join-Path $env:USERPROFILE '.cache/codex-runtimes/codex-primary-runtime/dependencies/python/python.exe'
    $Python=if(Test-Path $bundled){$bundled}else{'python'}
}
& $Python -B "$PSScriptRoot/BakeReusableOakPilot.py" prepare
if ($LASTEXITCODE) {exit $LASTEXITCODE}
$members=Join-Path $repo 'assets/source/white-oak-forked-spray'
$inputs=Get-Item "$assets.glb","$assets.treelod","$assets.leaves.fixedlod","$assets.spray-wood.fixedlod",
    "$members/WhiteOak-Bough07.txt","$members/WhiteOak-Bough08.txt","$build/Release/BakeSprayBough.exe"
$grid=Get-Item "$trial/grid-native.treelod" -ErrorAction SilentlyContinue
if ($Rebake -or !$grid -or ($inputs | Where-Object LastWriteTimeUtc -gt $grid.LastWriteTimeUtc)) {
    $pending="$trial/grid-$PID.treelod"
    & "$build/Release/BakeSprayBough.exe" --bough-grid "$assets.glb" "$assets.leaves.fixedlod" "$assets.spray-wood.fixedlod" `
        "$members/WhiteOak-Bough07.txt" "$members/WhiteOak-Bough08.txt" $pending
    if ($LASTEXITCODE) {exit $LASTEXITCODE}
    Move-Item -LiteralPath $pending -Destination "$trial/grid-native.treelod" -Force
}
$jobs=Get-Content "$trial/pilot-jobs.json" -Raw | ConvertFrom-Json
$missing=$jobs | Where-Object {!(Test-Path "$build/oak-assembly-step/$($_.output)/done.txt")}
if ($Rebake -or $missing) {
    $project=Join-Path $build 'unreal-nanite-oak/FoliageCreation.uproject'
    & "$PSScriptRoot/BuildOakAssemblyBench.ps1" -Unreal $Unreal -Project $project
    if ($LASTEXITCODE) {exit $LASTEXITCODE}
    $old=$env:OAK_BAKE_JOBS
    $previousForce=$env:OAK_BAKE_FORCE
    try {
        $env:OAK_BAKE_JOBS="$trial/pilot-jobs.json"
        if ($Rebake) {$env:OAK_BAKE_FORCE='1'}
        $log=Join-Path $trial "pilot-editor-$PID.log"
        $editorArgs=@("`"$project`"", '-EnablePlugins=PythonScriptPlugin,OakAssemblyBench',
            "-ExecutePythonScript=`"$PSScriptRoot/RunOakAssemblyBake.py`"", "-abslog=`"$log`"",
            '-unattended','-nullrhi','-NoSound','-NoSplash','-NoP4','-NoCrashDialog','-DDC=NoShared')
        $process=Start-Process -FilePath "$Unreal/Engine/Binaries/Win64/UnrealEditor-Cmd.exe" `
            -ArgumentList $editorArgs -WindowStyle Hidden -Wait -PassThru
        $complete=(Test-Path -LiteralPath $log) -and (Select-String -Quiet -LiteralPath $log -Pattern 'OAK_BENCH ASSEMBLIES_COMPLETE')
        if ($process.ExitCode -ne 0 -or !$complete) {
            throw "Offline spray bake exited $($process.ExitCode), completion marker $complete. See $log."
        }
    } finally {$env:OAK_BAKE_JOBS=$old; $env:OAK_BAKE_FORCE=$previousForce}
}
& $Python -B "$PSScriptRoot/BakeReusableOakPilot.py" package
exit $LASTEXITCODE
