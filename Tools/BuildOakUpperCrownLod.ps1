param(
    [ValidateSet('fork','crown','lit','sunward','edge','grove')] [string]$Pose = 'grove',
    [ValidateSet(1,4)] [int]$Trees = 4,
    [ValidateRange(0.1,100000)] [double]$Distance,
    [double]$Yaw, [ValidateRange(0.001,16)] [double]$PixelError = 1,
    [switch]$View, [switch]$Rebake, [switch]$Unrestricted, [string]$Capture, [string]$Python
)
$ErrorActionPreference = 'Stop'
if (!$Python) {
    $bundled = Join-Path $env:USERPROFILE '.cache/codex-runtimes/codex-primary-runtime/dependencies/python/python.exe'
    $Python = if (Test-Path $bundled) { $bundled } else { 'python' }
}
$opts = @('--pose', $Pose, '--trees', $Trees, '--pixel-error', $PixelError)
if ($PSBoundParameters.ContainsKey('Distance')) { $opts += @('--distance', $Distance) }
if ($PSBoundParameters.ContainsKey('Yaw')) { $opts += @('--yaw', $Yaw) }
if ($Rebake) { $opts += '--rebake' }
if ($View) { $opts += '--view' }
if ($Unrestricted) { $opts += '--unrestricted' }
if ($Capture) { $opts += @('--capture', $Capture) }
& $Python -B "$PSScriptRoot/OakLightingStudy/optimize_crown.py" @opts
exit $LASTEXITCODE
