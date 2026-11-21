param([string]$Unreal = 'C:/Program Files/Epic Games/UE_5.8',
      [string]$Project = "$PSScriptRoot/../build/unreal-nanite-oak/FoliageCreation.uproject")
$ErrorActionPreference = 'Stop'
$projectFile = (Resolve-Path -LiteralPath $Project).Path
$dest = Join-Path (Split-Path $projectFile) 'Plugins/OakAssemblyBench'
New-Item -ItemType Directory -Force $dest | Out-Null
Copy-Item -Path "$PSScriptRoot/OakAssemblyBench/*" -Destination $dest -Recurse -Force
$env:OAK_ENGINE_REPO = (Resolve-Path "$PSScriptRoot/..").Path
& "$Unreal/Engine/Build/BatchFiles/Build.bat" UnrealEditor Win64 Development "-Project=$projectFile" "-Plugin=$dest/OakAssemblyBench.uplugin" -Module=OakAssemblyBench -Module=OakGameTiming -NoHotReload
if ($LASTEXITCODE) { exit $LASTEXITCODE }
# A module-only installed-engine build links the DLL without writing the discovery manifest.
$id = (Get-Content "$Unreal/Engine/Binaries/Win64/UnrealEditor.modules" -Raw | ConvertFrom-Json).BuildId
@{BuildId=$id; Modules=@{OakAssemblyBench='UnrealEditor-OakAssemblyBench.dll'; OakGameTiming='UnrealEditor-OakGameTiming.dll'}} |
    ConvertTo-Json | Set-Content "$dest/Binaries/Win64/UnrealEditor.modules" -Encoding utf8
