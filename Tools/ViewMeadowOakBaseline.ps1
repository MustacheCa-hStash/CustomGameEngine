param(
    [ValidateSet('edge','sun','roots','layout','blade')] [string]$Pose = 'edge',
    [switch]$OpenSky,
    [switch]$ShadowVisibility,
    [switch]$Capture,
    [ValidateSet('Before','Shadows','Material','After')] [string]$Pass = 'Before',
    [string]$Python
)
$ErrorActionPreference = 'Stop'
if (!$Python) {
    $bundled = Join-Path $env:USERPROFILE '.cache/codex-runtimes/codex-primary-runtime/dependencies/python/python.exe'
    $Python = if (Test-Path $bundled) { $bundled } else { 'python' }
}
$mode = if ($Capture) { 'capture' } else { 'view' }
$options = @($mode, '--pose', $Pose)
if ($OpenSky) { $options += '--open' }
if ($ShadowVisibility) { $options += @('--lighting', 'shadow-visibility') }
$scriptName = 'oak_baseline.py'
if ($Pass -ne 'Before') {
    $scriptName = 'refine_lighting.py'
    $options += @('--pass', $Pass.ToLowerInvariant())
}
& $Python -B "$PSScriptRoot/MeadowGrassStudy/$scriptName" @options
exit $LASTEXITCODE
