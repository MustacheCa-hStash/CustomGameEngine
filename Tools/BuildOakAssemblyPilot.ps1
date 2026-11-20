param([string]$Python, [switch]$Rebake, [string]$Unreal = 'C:/Program Files/Epic Games/UE_5.8')
$ErrorActionPreference = 'Stop'
$root = (Resolve-Path "$PSScriptRoot/..").Path
$build = Join-Path $root 'build'
$cache = Join-Path $build 'CMakeCache.txt'
$cmake = (Select-String '^CMAKE_COMMAND:INTERNAL=(.*)' $cache).Matches.Groups[1].Value
& $cmake --build $build --config Release --target CustomGameEngine --parallel 6
if ($LASTEXITCODE) { exit $LASTEXITCODE }
if (!$Python) {
    $bundled = Join-Path $env:USERPROFILE '.cache/codex-runtimes/codex-primary-runtime/dependencies/python/python.exe'
    $Python = if (Test-Path $bundled) { $bundled } else { 'python' }
}
if ($Rebake) {
    $project = Join-Path $build 'unreal-nanite-oak/FoliageCreation.uproject'
    $output = Join-Path $build 'oak-assembly-step'
    & "$PSScriptRoot/BuildOakAssemblyBench.ps1" -Unreal $Unreal -Project $project
    if ($LASTEXITCODE) { exit $LASTEXITCODE }
    function Invoke-OakReduction($Name, $Jobs) {
        $jobFile = Join-Path $output "pilot-$Name-jobs.json"
        ConvertTo-Json -InputObject $Jobs -Depth 4 | Set-Content $jobFile -Encoding ascii
        $log = Join-Path $output "pilot-$Name-editor.log"
        $previousJobs = $env:OAK_BAKE_JOBS
        try {
            $env:OAK_BAKE_JOBS = $jobFile
            & "$Unreal/Engine/Binaries/Win64/UnrealEditor-Cmd.exe" $project '-EnablePlugins=PythonScriptPlugin,OakAssemblyBench' `
                "-ExecutePythonScript=$PSScriptRoot/RunOakAssemblyBake.py" "-abslog=$log" `
                '-unattended' '-nullrhi' '-NoSound' '-NoSplash' '-NoP4' '-NoCrashDialog' '-DDC=NoShared' | Out-Null
            if ($LASTEXITCODE -or !(Select-String -Quiet -LiteralPath $log -Pattern 'OAK_BENCH ASSEMBLIES_COMPLETE')) {
                throw "The offline editor bake failed. See $log."
            }
        } finally {
            $env:OAK_BAKE_JOBS = $previousJobs
        }
    }
    & $Python -B "$PSScriptRoot/PrepareOakAssembly.py" prepare
    if ($LASTEXITCODE) { exit $LASTEXITCODE }
    Invoke-OakReduction 'shared' @(
        @{input='wood.mesh'; arm='epic'; ratio=0.5; output='epic-0.5'}
        @{input='wood-part.mesh'; arm='custom'; ratio=0.5; output='part-custom-0.5'}
    )
    & $Python -B "$PSScriptRoot/PrepareOakAssembly.py" seeds
    if ($LASTEXITCODE) { exit $LASTEXITCODE }
    Invoke-OakReduction 'seeded' @(
        @{input='instanced-custom-01.mesh'; arm='epic'; ratio=0.5; output='seed-custom01-epic'}
    )
}
foreach ($file in 'oak-branch-step-final/grid/assets/WhiteOak-ForkedCanopy.treelod',
                  'oak-assembly-step/seed-custom01-epic/level01.mesh','oak-assembly-step/epic-0.5/level04-paired.mesh',
                  'unreal-nanite-oak/spray-and-camera.json') {
    if (!(Test-Path (Join-Path $build $file))) { throw "Missing matched benchmark input: $file. See Documentation/OAK_ASSEMBLY_41_100.md." }
}
& $Python -B "$PSScriptRoot/PackageOakAssembly.py"
exit $LASTEXITCODE
