# Custom Game Engine

The viewer loads a Blender-exported mesh and draws it on a plane through Direct3D 12. Left-drag to orbit, scroll to zoom, and press `R` to reset the camera. Press `M` to compare 4× MSAA with single-sample rendering. On the oak, press `P` to pause or resume the wind. The title bar shows both states. The viewer starts at 4× when the GPU supports it.

## Build and run

Use Visual Studio 2022 Build Tools with x64 MSVC, CMake, and a Windows SDK. From the project directory in PowerShell, run the build with the CMake bundled in the installed Build Tools:

```powershell
$engineCmake = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
& $engineCmake -S . -B build -G "Visual Studio 17 2022" -A x64
if ($LASTEXITCODE -eq 0) { & $engineCmake --build build --config Debug }
```

Launch the viewer:

```powershell
.\build\Debug\CustomGameEngine.exe
```

The Scene Inspector opens on the left. Use its **Scene** menu to switch between Cube, Oak, and Grove. Press `F1` to hide or reopen the panel. The top-right performance readout stays visible.

### Inspect a scene

- **Scene controls** has camera presets, sun direction, wind, MSAA, and the grove tree count.
- **Outliner** lets you select the model, ground, or a particular tree placement in the grove. In **Object**, choose Bark or Foliage to open its **Mesh** details. Follow its assigned **Material**, then open the **Texture** preview to see the bound image and mip count.
- **Material** changes apply live. Oak and Grove share their oak materials, so a foliage cutoff edit in Oak applies to every tree in Grove, including its shadow. The cutoff uses the existing texture mips, whose alpha coverage was built using the source material cutoff.

### Saved viewer settings

The viewer reads and writes `viewer-settings.ini` in the project root beside `CMakeLists.txt`. Debug and Release builds from the same source directory share this file. The viewer saves after a short pause in edits and flushes pending changes on close.

The file is UTF-8 INI with `key = value` entries, decimal numbers, `true` or `false`, and full-line comments starting with `;` or `#`.

- `[viewer]` stores the global MSAA preference.
- `[scene.cube]`, `[scene.oak]`, and `[scene.grove]` store sun angles, wind strength and pause, all four bias values, and the tree count for Grove. Each scene has its own `.mesh.<name>` sections for cast and receive flags and a `.ground` section for ground flags and color.
- `[asset.oak.material.<name>]` stores each oak material’s base RGB, alpha cutoff, and backlighting where editable. Oak and Grove read the same sections. Cube has its own asset sections. Imported GLBs use sections keyed by their normalized absolute paths.

Mesh and material keys use the imported names, with reserved characters percent-escaped and duplicate names numbered by occurrence. Keep names unique and consistent across exports to retain their settings. Missing or invalid fields use their scene, hardware, or imported material defaults. An unsupported file version uses defaults for the entire file.

**Restore default bias** saves the four original bias values for the active scene. To reset all saved settings, close the viewer and launch `CustomGameEngine.exe --reset-settings` or remove `viewer-settings.ini` while the viewer is closed. Camera pose and presets, the active scene, the Grove route, wind animation phase, and inspector layout and selection are session-only. The viewer still accepts `--oak` and `--grove` to choose the starting scene.

### Reload mesh and shadow shaders

Save an edit to [Mesh.hlsl](shaders/Mesh.hlsl), then open **Shaders → Reload shaders** in the Scene Inspector. The viewer continues drawing while DXC compiles `VSMain`, `PSMain`, `ShadowVS`, and `ShadowPS` as Shader Model 6.0. The renderer creates the single-sample camera, shadow, and supported 4× MSAA camera pipelines with the active draw bias. It swaps them together between frames after every pipeline succeeds. On failure, the diagnostics show the failing stage and DXC's file, line, and column when available, or the pipeline operation and D3D12 error code. Use **Copy diagnostics** to copy the full log.

The executable looks for `shaders/Mesh.hlsl` under the source directory recorded by CMake. Includes can resolve relative to that shader directory. It loads `dxcompiler.dll` from beside the configured `DXC_EXECUTABLE`, normally the selected Windows SDK's x64 bin directory. A custom DXC install should have its matching `dxcompiler.dll` and `dxil.dll` in that directory. The diagnostics show the exact paths. If the source or compiler is unavailable, the current shaders stay active and the panel reports the failure. Startup always uses the bytecode embedded by the normal build. Reloaded bytecode lives in memory for that run.

Reload supports edits such as backlighting, cutout logic, wind math, and shadow comparisons that fit the existing shader interfaces. Changes to C++ constant layouts, register bindings, vertex inputs, the root signature, or pipeline state need a coordinated C++ and HLSL rebuild. The HUD shader still uses the normal build.

### Diagnose shadows

**Shadow diagnostics** edits the active scene live. **Restore default bias** resets and saves all four bias values for that scene.

| Control | Default | What it changes |
| --- | ---: | --- |
| Constant depth bias (draw) | 32 | Integer multiplier of a D32 float depth step. The step depends on the primitive's maximum depth. |
| Slope-scaled depth bias (draw) | 1.00× | Multiplier of the maximum depth slope per shadow-map pixel. Faces viewed at a grazing angle from the light get a larger offset. |
| Shadow sampling bias (receive) | 0.50 texels | Uses the world width of a shadow-map texel as a distance along the light's depth axis. The shader subtracts the converted offset from the receiver's normalized light depth before comparison. |
| Normal bias (receive) | 0.00 texels | Offsets the receiver's shadow lookup along the sun-facing world normal before projection, changing both the sampled location and the comparison depth. |

One normal-bias texel is the larger world-space dimension of a texel in the fitted orthographic shadow map. This is the same distance scale used by sampling bias. The normal offset is that size times the slider value times `sqrt(1 - dot(N, L)^2)`, where `N` is the normalized, deformed world normal and `L` points toward the sun. The offset is zero when the sun hits head-on and reaches the displayed maximum at grazing incidence. For backlit foliage the lookup uses the opposite normal so the offset still points into the sun-facing hemisphere. Only the receiver's lookup moves. Camera geometry and caster geometry use the same wind deformation as before.

Direct3D adds both draw terms and caps the positive sum at **0.002 normalized light depth**. Once a face reaches that cap, increasing either draw term has no further effect on that face. Both receiver biases operate independently of the draw cap and can be combined. Higher bias can clear self-shadowing bands and can also separate shadows from contact points or erase thin, legitimate shadows. [Direct3D depth-bias formula](https://learn.microsoft.com/en-us/windows/win32/direct3d11/d3d10-graphics-programming-guide-output-merger-stage-depth-bias)

Use **Mesh → Cast shadows** to include that mesh in the shadow map, and **Receive shadows** to control whether its shading samples the map. Bark and foliage have separate settings. A Grove setting applies to that mesh on every tree in the scene. Ground starts with casting off and receiving on. The camera's MSAA toggle changes camera edge sampling while the shadow map stays single-sample.

The build copies both sample GLBs into the `assets` folder alongside the executable. Use `--oak` to start in the named Oak scene. A GLB path opens a separate Imported GLB scene:

```powershell
.\build\Debug\CustomGameEngine.exe ".\assets\LowBranchedWhiteOakLOD0.glb"
```

### Grove and performance readout

```powershell
.\build\Debug\CustomGameEngine.exe --grove
```

The grove starts with 100 trees. Press `1`, `2`, `3`, or `4` for 1, 25, 100, or 400 trees. Each preset uses the same placements for the trees it shares with the other presets. Press `B` to start or stop the same 24-second orbit, or `R` to return to manual control. Changing the tree count during the route restarts it. Keep the MSAA setting and resolution fixed for comparisons, and pause the wind with `P` if you want the same pose across runs.

The top-right readout averages over roughly half a second:

- **FPS** is the rate of completed frames, including presentation and the frame fence. VSync can cap it at the display refresh rate.
- **CPU ms** measures renderer preparation, command recording, and submission, before the calls to present and wait for the GPU.
- **GPU ms** uses D3D12 timestamps around the shadow, camera, resolve, and HUD work. It excludes presentation. Dashes indicate that there is no timestamp result.

With default shadow settings, the grove submits all selected trees at LOD0 with two indexed instanced draws per pass, one for bark and one for foliage. The ground adds one camera draw. This is the baseline before LOD or visibility culling.

Use a self-contained glTF 2.0 binary with uncompressed static triangle meshes, positions, and normals. The viewer applies scene-node transforms and fits larger assets using their world-space bounds. It reads each primitive's base-color factor and optional embedded PNG or JPEG, with UV set 0 or 1 and a fixed trilinear wrap sampler. It renders opaque and alpha-mask materials under a fixed sun. A missing or invalid file opens an error dialog showing its path.

The build finds DXC in the selected Windows SDK and compiles the shaders. The shader bytecode is embedded in the executable. If you use a separate DXC installation, pass `-DDXC_EXECUTABLE=<path-to-dxc.exe>` at configure time. The GLB parser is [cgltf v1.15](https://github.com/jkuhlmann/cgltf/releases/tag/v1.15). The inspector uses [Dear ImGui v1.92.5](https://github.com/ocornut/imgui/releases/tag/v1.92.5) with its Win32 and D3D12 backends. Both dependencies are vendored with their license notices.

## Blender sources

- The [cube project](assets/source/cube.blend) is the editable source for `assets/cube.glb`. It was recreated from the original Blender export script and produces an identical GLB.
- The [low-branched white oak LOD0 project](../FirstSettlers/ArtSource/WoodlandEdgeLOD0/LowBranchedWhiteOak/LowBranchedWhiteOakLOD0.blend) lives in the FirstSettlers art repository with its textures packed. The engine GLB uses the original 4,192 triangles and UVs. Its materials embed `BarkAlbedo.png` and a 2048 × 2048 export of `CrownAlbedoBase.png` with its alpha channel. The foliage uses the FirstSettlers material's 0.30 cutoff and renders on both sides.

## Trace a frame

1. `loadGlb` in `src/GltfLoader.cpp` reads positions, normals, indices, and up to two UV sets from the file. It resolves world transforms and reads the base color and alpha mode from each primitive's material. For textures, Windows Imaging Component decodes the embedded image to RGBA. The loader builds mip levels and adjusts the alpha of cutout mips to keep the fraction of texels above the cutoff close to the original.
2. `uploadMesh` copies the geometry into GPU-readable vertex and index buffers. The grove shares those buffers across trees and reads position, yaw, and scale from a separate instance buffer. `uploadTexture` uses a staging buffer to copy each mip into a GPU texture and creates a shader-resource view for sampling.
3. The renderer writes the camera, light, and wind values once per frame. Both vertex shaders call `bendPosition` with the same wind offset. The bend keeps the bottom 12% of the asset fixed and grows with height. In this single-asset viewer, a model with two-sided leaf cutouts enables wind for its bark and foliage together.
4. `drawShadowMap` renders the bent asset from the sun's direction into a 2048 × 2048 depth map. The foliage uses the same UVs, alpha texture, and cutoff as the camera pass. The ground receives shadows and is skipped when drawing shadow casters.
5. The camera pass draws the scene from the orbit view. The pixel shader applies the leaf cutout, then compares each surface's light-space depth with the shadow map. A filtered comparison controls how much direct light and leaf backlighting reach the surface. Ambient fill remains visible in shade.
6. With 4× MSAA, camera drawing uses an offscreen color target and depth buffer that both have four samples per pixel. The renderer resolves the color samples into the swap-chain buffer. In single-sample mode it draws directly into the swap-chain buffer with a single-sample depth buffer. The shadow map uses one sample in either mode.
7. The renderer transitions the back buffer for presentation, submits the command list, and calls `Present`. It waits on a fence before reusing the command allocator and frame constants.

The startup check looks for four samples on both the color and depth formats at quality index zero, plus color resolve support. Each mode has a matching pipeline state. If 4× is unavailable, the viewer uses single-sample rendering and reports that in the title bar. Compare a slanted cube edge from a fixed camera position to see what the resolve changes.

Orbit to the side of the oak to watch the crown and its shadow sway together. `P` freezes the pose and shadow while leaving the camera controls active. The [engine plan](Documentation/ENGINE_PLAN.md) tracks the next foliage milestones.
