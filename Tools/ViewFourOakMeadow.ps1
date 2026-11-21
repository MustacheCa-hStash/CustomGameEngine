param(
    [ValidateSet('canopy', 'clearing', 'downhill', 'foreground', 'bough')] [string]$Pose = 'canopy',
    [ValidateSet('before', 'frustum', 'culled', 'meadow', 'reference', 'shadow', 'ground', 'both', 'bank', 'original', 'lod', 'full', 'stems', 'sparse', 'sparse-contact', 'patch-contact', 'patch-near', 'sward-contact', 'sward-near', 'geometry', 'cards', 'tuft-reference', 'tuft-pilot', 'tuft-slices', 'tuft-ribbons', 'tuft-all-cards', 'distant-reference', 'distant-optimized', 'adaptive-oaks', 'meadow-aa', 'daylight')] [string]$Variant = 'daylight',
    [ValidateSet('off','msaa','hdr','split','temporal')] [string]$AA = 'msaa',
    [ValidateSet('original','daylight','bounce')] [string]$Lighting = 'bounce',
    [switch]$Prepare,
    [switch]$Capture
)
$ErrorActionPreference = 'Stop'
$python = Join-Path $env:USERPROFILE '.cache/codex-runtimes/codex-primary-runtime/dependencies/python/python.exe'
if (!(Test-Path $python)) { $python = 'python' }
$builder = Join-Path $PSScriptRoot 'MeadowGrassStudy/grass_tiles.py'
$manifest = Join-Path $PSScriptRoot '../build/meadow-tiles/visual/scene.json'
$selector = '--variant'
$selectedVariant = $Variant
if ($Variant -in @('reference','shadow','ground','both')) {
    $builder = Join-Path $PSScriptRoot 'MeadowGrassStudy/far_shadow.py'
    $manifest = Join-Path $PSScriptRoot '../build/meadow-far-shadow/scene/assets/MeadowCover.png'
    $selector = '--arm'
}
if ($Variant -in @('bank','original')) {
    $builder = Join-Path $PSScriptRoot 'MeadowGrassStudy/camera_meadow.py'
    $manifest = Join-Path $PSScriptRoot '../build/meadow-camera/scene/CameraMeadow.exe'
    $selector = '--arm'
}
if ($Variant -in @('lod','full')) {
    $builder = Join-Path $PSScriptRoot 'MeadowGrassStudy/camera_lod.py'
    $manifest = Join-Path $PSScriptRoot '../build/meadow-camera-lod/scene/MeadowLod.exe'
    $selector = '--arm'
}
if ($Variant -eq 'stems') {
    $builder = Join-Path $PSScriptRoot 'MeadowGrassStudy/clearing.py'
    $manifest = Join-Path $PSScriptRoot '../build/meadow-clearing/scene/Clearing.exe'
    $selector = '--arms'
}
if ($Variant -in @('sparse','sparse-contact','patch-contact','patch-near','sward-contact','sward-near')) {
    $builder = Join-Path $PSScriptRoot 'MeadowGrassStudy/sparse.py'
    $manifest = Join-Path $PSScriptRoot '../build/meadow-sparse/scene/SparseMeadow.exe'
    $selector = '--arm'
}
if ($Variant -in @('geometry','cards')) {
    $builder = Join-Path $PSScriptRoot 'MeadowGrassStudy/cards.py'
    $manifest = Join-Path $PSScriptRoot '../build/meadow-cards/scene/MeadowCards.exe'
    $selector = '--arm'
}
if ($Pose -eq 'bough' -and $Variant -ne 'daylight') { throw 'The bough camera requires the daylight variant.' }
if ($Variant -eq 'daylight') {
    if (!$PSBoundParameters.ContainsKey('AA')) { $AA = 'split' }
    $builder = Join-Path $PSScriptRoot 'MeadowGrassStudy/meadow_daylight.py'
    $manifest = Join-Path $PSScriptRoot '../build/meadow-daylight/MeadowDaylight.exe'
} elseif ($Variant -eq 'meadow-aa') {
    $builder = Join-Path $PSScriptRoot 'MeadowGrassStudy/meadow_aa.py'
    $manifest = Join-Path $PSScriptRoot '../build/meadow-aa/MeadowAA.exe'
} elseif ($Variant -eq 'adaptive-oaks') {
    $builder = Join-Path $PSScriptRoot 'MeadowGrassStudy/adaptive_oaks.py'
    $manifest = Join-Path $PSScriptRoot '../build/meadow-adaptive/AdaptiveMeadow.exe'
} elseif ($Variant.StartsWith('distant-')) {
    $builder = Join-Path $PSScriptRoot 'MeadowGrassStudy/distant.py'
    $manifest = Join-Path $PSScriptRoot '../build/meadow-distant/DistantMeadow.exe'
    $selector = '--arm'
    $selectedVariant = $Variant.Substring(8)
} elseif ($Variant.StartsWith('tuft-')) {
    $builder = Join-Path $PSScriptRoot 'MeadowGrassStudy/tuft_models.py'
    $manifest = Join-Path $PSScriptRoot '../build/meadow-tuft-models/MeadowTufts.exe'
    $selector = '--arm'
    $selectedVariant = $Variant.Substring(5)
} elseif ($Pose -eq 'foreground') {
    throw 'The foreground camera is available with the tuft and distant grass variants.'
}
if ($Prepare -or !(Test-Path $manifest)) {
    & $python -B $builder prepare
    if ($LASTEXITCODE) { exit $LASTEXITCODE }
}
$mode = if ($Capture) { 'capture' } else { 'view' }
if ($Variant -eq 'daylight') {
    & $python -B $builder $mode --pose $Pose --aa $AA --lighting $Lighting
} elseif ($Variant -eq 'meadow-aa') {
    & $python -B $builder $mode --pose $Pose --aa $AA
} elseif ($Variant -eq 'adaptive-oaks') {
    & $python -B $builder $mode --pose $Pose
} else {
    & $python -B $builder $mode $selector $selectedVariant --pose $Pose
}
exit $LASTEXITCODE
