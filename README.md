# Custom Game Engine

The viewer loads a Blender-exported mesh and draws it on a plane through Direct3D 12. Left-drag to orbit, middle-drag to pan the orbit focus, scroll to zoom, and press `R` to reset the camera. You can move to within 0.02 scene units of the focus or enter a distance in the inspector. Viewport camera input pauses a playing route for manual inspection. Press `M` to compare 4× MSAA with single-sample rendering. On the oak, press `P` to pause or resume the wind. The title bar shows both states. The viewer starts at 4× when the GPU supports it.

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

The viewer and scene captures use `assets/MeadowDaySky.hdr`, a scene-linear daytime bake of the **Meadow Day** sky from FirstSettlers' **ForestVarietyPilot** scene. The camera draws the panorama behind scene geometry. Mesh and voxel shading evaluate its upper hemisphere through nine RGB spherical-harmonic coefficients, calibrated to the previous ambient luminance on upward-facing surfaces. The visible sun disk follows the directional light and is drawn separately from the panorama.

**Probe lighting** in Scene controls enables the experimental world-space field. The field stores a six-direction diffuse correction to open sky and uses compact directional distance moments to weight neighboring probes. Compute shaders trace a coverage proxy of the source geometry and can add one diffuse bounce from the ground. Mesh and voxel foliage sample the same field. Each sweep updates probes near the camera frustum first. Wind-driven probe work waits for an eighth of a coverage voxel of displacement from the last refreshed pose or eight rendered frames. Ground and probe rays share the displaced volume, and routine ground updates spread across eight refreshes. A wind pause starts a full sweep at the held pose.

### Windowless scene capture

Run the same engine renderer from PowerShell to save a still or an image sequence. Capture mode uses D3D12 and does not create a viewer window. Use a new output path for each run.

```powershell
.\build\Debug\CustomGameEngine.exe --capture build\captures\cube.png --width 1280 --height 720 --msaa 4
.\build\Debug\CustomGameEngine.exe --capture build\captures\oak-orbit --scene branch --camera close-up --frames 180 --fps 60 --orbit-degrees 18 --msaa 4
.\build\Debug\CustomGameEngine.exe --capture build\captures\oak-wind --scene modeled-oak --frames 120 --fps 60 --animate-wind
```

- The default still is Cube at 3840 × 2160 and 1× MSAA. The viewer renders at 3840 × 2160 and fits its window to the desktop work area. The title bar shows the render size. `--scene` also accepts `oak`, `modeled-oak`, `oak-walk`, `grove`, `branch`, `branch-grid`, `branch-orbit`, `branch-transition`, `forked-oak`, or a GLB path. Camera presets are `overview`, `close-up`, `under-canopy`, and `distant`.
- Use `--yaw-degrees`, `--pitch-degrees`, and `--distance` to override the preset pose. `--orbit-degrees` is the total yaw change. Use `--end-distance` and `--end-pitch-degrees` for the final distance and pitch of a frame sequence. Use `--lod auto`, `--lod source`, or a baked triangle level such as `--lod 6` for LOD comparisons.
- Add `--geometry-mask` to capture the visible asset on black for screen coverage measurements. It colors the selected triangles blue and voxels amber and respects material cutouts.
- Wind holds at `--wind-time` (zero seconds by default) and advances only with `--animate-wind`. Use `--wind-strength` to set the amplitude, or `--sun-azimuth`, `--sun-elevation`, `--backlighting`, and `--no-leaf-shadow-reception` for lighting comparisons. `--settings path.ini` applies a saved scene configuration before explicit overrides. Capture leaves the INI untouched.
- Shaded frames use a linear RGBA16F camera target, linear MSAA resolve, and a shared SDR display pass. Set manual exposure with `--exposure-ev` (default 0 EV), and add `--linear-hdr scene.dds` to a shaded still capture to save the half-float scene alongside its PNG. The DDS stores the resolved scene values before exposure and tone mapping. Encoded diagnostic views retain their original values.
- Use `--lighting sun-only`, `--lighting sky-only`, `--lighting transmission`, or `--lighting shadow-visibility` to inspect lighting components. `--sky-only` is an alias for `--lighting sky-only`. Sun-only and sky-only isolate the light on geometry. Transmission and shadow visibility use a black backdrop.
- Use `--review-capture` with the same 4K still-capture options, omitting the output path, to open that pose and lighting in the interactive viewer. The review launch reads its settings from the command line and any explicit `--settings` file. Changes during the review stay in that session.
- The Forked White Oak scene and its one- and four-tree review views use canopy-filtered skylight plus one ground bounce by default. Captures inherit the scene setting. Use `--probe-gi off`, `--probe-gi sky`, or `--probe-gi bounce` to explicitly select open sky, canopy-filtered sky, or canopy-filtered sky plus ground bounce. The triangle performance suite pins open sky for historical comparisons. [Oak voxel and lighting measurements](Documentation/OAK_VOXEL_TRANSITION_AND_LIGHTING.md)
- `--probe-walk new-directory` runs at 3840 × 2160 with 4× MSAA and 0 EV. It records the GPU adapter, the grove approach, convergence after a sun change and a wind pause, the branch mesh/voxel sweep, and matched Forked White Oak captures and timings with per-group, bough, and hierarchy cuts. Install the Forked White Oak geometry bakes and the branch eighth-meter coverage and normal bakes from the `oak_branch_normals` target.
- Use `--probe-wind-voxels 0` to refresh on every moving frame or pass a threshold up to one coverage voxel. `--probe-wind-benchmark new-directory` tests 0, ⅛, ¼, and ½ voxel thresholds at 4K on the Forked White Oak with a fixed under-canopy camera and a crown orbit. It records GPU timings and matched linear frames against a fresh field. Append `--timings-only` to collect GPU timings without image readbacks.
- A sequence includes `frame-000000.png` onward. The engine renders at the requested playback timestep regardless of how long each frame takes. `frames.csv` records frame time, camera pose, wind time, and MSAA after all images finish. Use a different output directory to retry an interrupted sequence.

Use the lossless PNGs for pixel comparisons. To make a playback copy with FFmpeg, set its input frame rate to the capture rate:

```powershell
ffmpeg -framerate 60 -i build/captures/oak-orbit/frame-%06d.png -pix_fmt yuv420p build/captures/oak-orbit.mp4
```

### Inspect a scene

- **Scene controls** has camera presets, sun direction, exposure, wind, MSAA, and the grove tree count.
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

The build copies the sample GLBs into the `assets` folder alongside the executable. Use `--oak` to start in the named Oak scene. A GLB path opens a separate Imported GLB scene:

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

Open **Forked White Oak** from the Scene menu or start with `--forked-oak`. Its source GLB contains the PVE trunk and boughs and places the shared spray through `EXT_mesh_gpu_instancing`. The build bakes the spray's leaf and wood primitives once each and copies both sidecars alongside the GLB.

In the inspector, **Spray LOD** selects **Automatic**, **LOD 3**, or **Source**, sets the camera error, and toggles frustum culling. Automatic uses **Geometry hierarchy** when its bake is available. It can select trunk LODs and replace a range of sprays with a baked parent mesh. Leaves and twig wood select independently. At the ends of the hierarchy, **Per-group leaf and twig LOD** selects cuts within individual sprays and batches placements that share a cut.

The forked oak's leaf and wood `.fixedlod.txt` reports include direct comparisons for every pair of levels in their automatic ranges. The leaf table covers LOD 3–6 and the spray wood table covers LOD 0–5. Each row gives the maximum sampled surface error in each direction in mesh-local units. The leaf table also gives the symmetric open-rim error. Automatic selection uses the source-relative errors. For other bakes, pass `--transition-min-lod N` to `BakeFixedLod` to enable the table from level N through the coarsest baked level.

Forked oak leaf LOD 4 targets 33 triangles per leaf through `BakeFixedLod --lod4-component-triangles N`. With `--lod4-refinement-error F`, the baker retries with more triangles when a simpler candidate exceeds the sampled surface or rim error limit from source. A leaf that needs its regular LOD 4 keeps that geometry. The forked spray bakes 1,504 triangles across all 45 leaves at LOD 4.

Spray wood uses the optional `BakeFixedLod --shared-coarse-triangles N` recipe to produce LOD 5 from the original LOD 4. The `--shared-far-triangles N` option then reduces LOD 4 across the whole spray and checks its prior selection error. `--shared-far-uv-weight F` sets the UV weight for that step. The forked oak uses 950 triangles at wood LOD 4 and 422 at LOD 5. Both levels reuse the source vertex buffer. The inspector's **Rendering** panel reports how many placements selected the coarse wood, and capture CSVs give the full leaf and wood LOD counts for each pass.

Use **Spray LOD → Color by → Group** to see stable spatial placement groups or **LOD level** to see the active cut. The leaves and twig wood of a spray share the same group color. Both views show active parent geometry in amber. For a windowless capture, pass `--spray-color group` or `--spray-color lod`.

The [4K per-group benchmark](Documentation/OAK_LOCAL_GROUP_LOD_4K.md) compares the whole-spray and per-group cuts over the same five camera distances and includes a moving coverage check.

The bough prototype replaces 371 linked sprays when its projected error fits the camera budget. Its bake starts from the fitted LOD 6 leaf meshes and consolidates eligible quads into one area-preserving triangle. It simplifies the combined spray wood and checks sampled distances in both directions. Its membership follows bough 7 of the PVE skeleton and is tied to the exported GLB. The shadow pass checks its own light frustum and uses a 0.75-texel budget. Choose **Scene controls → Distant** for the 100 m comparison. **Rendering** reports draw counts, GPU times, group tests, individual tests, and the number of sprays represented by the active bough proxy.

Use `--bough-bake <file>` in a windowless Forked White Oak capture to compare a compatible alternative proxy. Add `--force-bough` with automatic LOD and bough traversal to hold the proxy active in the camera and shadow captures for a visual comparison. The automatic budget compares geometry error in render-target pixels and therefore depends on the actual render size.

For a source comparison, capture with `--scene forked-oak --lod source --spray-culling off`. For matched automatic runs, use `--scene forked-oak --lod auto --spray-culling on` and pass `--spray-mode flat`, `--spray-mode grouped`, or `--spray-mode bough`. Pass `--spray-local-lod off` for the whole-spray control with the same traversal. Add `--timing-samples 51` to a Release still capture for a sibling `.timings.csv` with GPU timestamps and submitted triangle counts. Timed frames exclude PNG readback and follow two warm-up frames.

In **Spray LOD → Tree submission**, **CPU direct** is the default. **CPU selection + indirect** batches the CPU's draw records. **GPU selection + indirect** traverses one tree part at a time. **GPU batched + indirect** queues spray ranges across the view and instances selected cuts across trees. Use `--spray-mode hierarchy --tree-submit direct|indirect|gpu|batched` in captures. The per-part GPU mode supports **Depth culling for twig wood** through `--tree-hzb on`. Use `--tree-count 16` with `--scene forked-oak` in captures and interactive review to place a stand at the same camera coordinates. Timings include camera and shadow counts and the separate GPU stages for batched mode. [GPU batching replay](Documentation/OAK_TREE_BATCH_4K.md)

With **GPU batched + indirect**, enable **Camera cluster frustum culling** to test smaller ranges inside spray and bough cuts that cross the edge of the camera. The capture equivalent is `--tree-submit batched --tree-clusters on`. It uses the existing projected error budget and reports cluster test time and rejected bounds in the timing CSV. [4K cluster frustum replay](Documentation/OAK_CLUSTER_FRUSTUM_4K.md)

**Two-pass camera occlusion** in the same menu uses the last frame's depth to defer likely-hidden leaf, twig, and trunk geometry. It retests every deferred range against the current frame. Use `--tree-submit batched --tree-occlusion on` for a capture with occlusion enabled. [4K occlusion replay](Documentation/OAK_TWO_PASS_OCCLUSION_4K.md)

The build finds DXC in the selected Windows SDK and compiles the shaders. The shader bytecode is embedded in the executable. If you use a separate DXC installation, pass `-DDXC_EXECUTABLE=<path-to-dxc.exe>` at configure time. The GLB parser is [cgltf v1.15](https://github.com/jkuhlmann/cgltf/releases/tag/v1.15). The inspector uses [Dear ImGui v1.92.5](https://github.com/ocornut/imgui/releases/tag/v1.92.5) with its Win32 and D3D12 backends. Both dependencies are vendored with their license notices.

## Blender sources

### Checkpoint 12B. Card Oak and Modeled Oak

Select **Card Oak** or **Modeled Oak** in the Scene menu. `--oak` still opens Card Oak, and `--modeled-oak` starts directly in Modeled Oak. The scenes share the same Overview, Close-up, and Under canopy framing. Card Oak retains its existing settings and shares materials with Grove. Modeled Oak has its own scene and material settings.

Modeled Oak has 5,297,600 leaf triangles and 425,778 bark triangles. Its full-detail GLB is about 212 MiB. It uses the existing indexed renderer for camera and shadow draws. Modeled Oak and Modeled Oak Walk share the bark albedo and an authored 512 × 512 leaf base-color bake with veins and UV noise. Source and baked levels sample the same leaf material with editable backlighting.

#### Modeled Oak Walk

Open **Modeled Oak Walk** from the Scene menu or launch with `--oak-walk`. **Walk and glance** walks straight towards the trunk at 1.5 m/s. It starts 55.5 m out, glances 31° to either side, and stops 3 m from the trunk. **Circle trunk** walks one level lap at an 8 m radius and keeps looking toward the trunk. Both routes use a 1.88 m eye height for a 2 m character. Press **B** to play or pause, **R** to reset, or drag **Route time** to inspect a held pose. The default view starts paused with wind strength zero.

In **Foliage LOD**, switch between **Automatic** and **Source GLB** at the same route time. The automatic chain uses LOD 0, 2, 3, 4, and 6 across 2,070 fixed groups. **Freeze cut** holds the selected levels while the camera moves, and **Color by → LOD level** shows the selection regions. Bark uses its original mesh. Both camera comparison modes default to the same LOD 4 triangle foliage shadow cut.

Build the full-crown sidecar with the optional `oak_crown_lod` target. For example, `cmake --build build/phase1 --config Release --target CustomGameEngine oak_crown_lod`. Run `--oak-walk-benchmark report.csv` for fixed route poses, paired Source and Automatic renders, and per-pass timings at 4× MSAA.

The bake retains all 132,440 leaves, including at LOD 6 with 264,880 triangles. With whole-tree culling alone, Automatic reduced the 55.5 m start from 5,297,600 to 264,880 foliage triangles and the median GPU camera pass from 0.737 to 0.167 ms. The comparison uses the same 1,456,840 foliage shadow triangles at every pose. Both routes cleared the source geometry in the eye-height trace, and Debug and Release produced identical preview captures and cut counts. The bake report is bundled beside the GLB.

**Foliage group frustum culling** is enabled by default for Automatic and the baked levels. Use **LOD 0** for a grouped full-detail comparison. The camera uses conservative group bounds that enclose both source and proxy vertices and account for the current wind. When the whole tree fits in the camera frustum, all of its groups are accepted together. **Rendering → Foliage groups** reports the camera counts. Single-map shadows test whole trees against the light frustum.

With the Automatic cut held at each pose, two RTX 5090 runs at 3200 × 1800 and 4× MSAA measured the following median camera times. The 40 paired validation captures per run matched pixel for pixel, including 1× and 4× MSAA, wind, forced baked levels, direct and indirect submissions, and camera-matched shadows.

| Walk pose | Groups drawn | Foliage triangles, whole tree → group culling | GPU camera, whole tree → group culling |
| --- | ---: | ---: | ---: |
| First side glance, 23.25 m | 765 / 2,070 | 3,308,648 → 1,131,782 | 0.440 → 0.261 ms |
| Second side glance, 11.25 m | 407 / 2,070 | 4,787,944 → 784,466 | 0.511 → 0.230 ms |
| End of walk, 3 m | 88 / 2,070 | 5,290,446 → 223,262 | 0.508 → 0.153 ms |

Run `--oak-walk-cull-benchmark report.csv` to reproduce the culling comparison. Saved results are in `build/phase1/oak-walk-cull-a.csv` and `build/phase1/oak-walk-cull-b.csv`.

#### Sun shadow cascade preview

Open **Shadow diagnostics → Sun shadow maps → Three cascades** in Modeled Oak Walk. **Single map** is the launch default and comparison control. **Cascade colors** shows red, green, and blue for camera depths up to 16, 48, and 120 m respectively. Gray marks surfaces beyond the shadow range. Turn the colors off to see the shaded result.

**Stabilize cascades** is on by default with Three cascades. Each 2048² map encloses a camera slice with a fixed-size square in light space and snaps its center to a whole shadow texel. The footprint stays fixed as the camera turns or walks with the same projection and bias settings. The inspector shows the world units per texel for each cascade. Turn stabilization off to compare against the original tight fit, which can use a smaller footprint for some views.

**Blend cascade boundaries** eases between the two maps at camera depths 14–16 m and 44–48 m. Shadow visibility fades to fully lit from 112 to 120 m. The colors show the primary cascade for each depth. **Wider shadow filter** averages four bilinear depth-comparison taps with a receiver-plane depth adjustment at each tap. The blend and filter default on with Three cascades and can be toggled independently.

All three maps share conservative caster depth bounds. In Modeled Oak Walk, **Cull cascade casters** defaults on and tests trees against each map's light-space frustum, including the blend overlap. The renderer accepts fully contained trees together. It tests blocks of 16 contiguous baked foliage groups on trees that cross a map edge, and tests individual groups only when a block crosses that edge. Bounds account for wind. Trees outside the camera frustum can still cast visible shadows. **Rendering → Shadow cascades** shows the tree, foliage group, triangle, and foliage call counts for each map.

**Coarsen shadow LOD per cascade** is an optional preview when **Shadow LOD** is LOD 4 for all casters. It uses LOD 6 for a tree in a map when the larger of its sampled surface and rim deviations from the source projects to at most 0.5 shadow texels. The threshold is evaluated at rest. The walk's original-size tree qualifies in the far cascade. The preview stays off by default because the LOD 6 trial changed self-shadowing on 15.1% of tree receiver pixels at the walk start by at least 8 on the 0–255 visibility scale. The corresponding ground figure was 0.039%.

Run `--sun-cascade-caster-benchmark report.csv` to compare the full caster lists, culling alone, and culling with automatic shadow LOD. Culling alone matched the full lists pixel for pixel in the walk, wind, and low-sun off-camera tests. In two Release runs on the RTX 5090 at 3200 × 1800 and 4× MSAA, it cut the end-of-walk shadow pass from about 0.305 to 0.175 ms. In the six-oak fixture, it cut shadow foliage from 26.22 million to 15.58 million triangles and GPU shadow time from 1.54 to 1.03 ms, while CPU draw-list time rose from about 0.06 to 0.12 ms. Foliage submission stayed at three API calls for that fixture. See `build/phase1/sun-cascade-casters-final.csv` and `build/phase1/sun-cascade-casters-repeat.csv`.

Run `--sun-cascade-transition-benchmark report.csv` for ground receiver probes, a sampling bias sweep at 54.6° and 25° sun elevation, and a fixed **Walk and glance** comparison. With the receiver-plane adjustment and existing 0.5-texel sampling bias, the ground self-shadow fixture recorded zero acne pixels at either sun angle, and the tree retained its contact shadow. At the 120 m cutoff, the average shadow visibility change across the two sampled screen rows dropped from 27.08 to 2.76 on a 0–255 scale. At the first side glance, median GPU camera time was 0.281 ms with hard boundaries and the narrow filter, and 0.294 ms with blending and the wider filter on an RTX 5090 at 3200 × 1800 and 4× MSAA. The shadow pass was about 0.305 ms in both cases. See `build/phase1/sun-cascade-transition-final.csv` and the paired captures beside it.

Run `--sun-cascade-stability-benchmark report.csv` to compare a 24° turn and a 0.60 m walk with wind off at 4× MSAA. A fixed observer samples the same ground points inside each cascade while the camera used to fit the maps moves. The tight fit changed 3.4–4.9% of ground samples per frame by at least 8 on the 0–255 shadow-visibility scale, averaged over each probe and motion. Stabilized maps recorded zero changes at that threshold. Release and Debug reports are in `build/phase1/sun-cascade-stability-a.csv` and `build/phase1/sun-cascade-stability-debug.csv`.

Run `--sun-cascade-benchmark report.csv --stable` for split-selection probes, a shadow check on all three maps, a single-map round trip, and a low-sun fixture with an off-camera oak. This diagnostic uses hard boundaries and the narrow filter. Omit `--stable` to run the tight-fit control. The CSVs and preview images are under `build/phase1/sun-cascade-*`.

For comparison, choose the same preset in each scene and match sun angles, MSAA, and shadow settings. Set wind strength to zero for an exact still comparison. Treat the initial comparison as geometry plus material because Card Oak uses baked crown textures. Compare silhouettes and visible gaps in Overview, leaf edges and overlap in Close-up, and branch visibility and transmission in Under canopy. Then orbit slowly with MSAA off and at 4× to compare shimmer. Record GPU frame time after the HUD settles at the same resolution and camera preset.

The full-tree Blender source is `FirstSettlers/ArtSource/WoodlandEdgeHighPoly/LowBranchedWhiteOak.blend`. To regenerate the full-tree GLB, use the command below with that source and `assets/LowBranchedWhiteOak-Modeled.glb` as the output.

### Checkpoint 12A. Modeled oak branch

Select **Oak Branch** from the viewer's **Scene** menu, or launch `build\Debug\CustomGameEngine.exe --branch` to start there. The branch saves settings under `[scene.oak_branch]` and `[asset.oak_branch.material.<name>]`. Use **Close-up** or **Under canopy**, pause wind with **P**, and inspect the separate bark and modeled-leaf meshes. In the leaf **Material** view, adjust **Backlighting** or reload a change to the transmission term in `Mesh.hlsl`. The leaves cast shadows from their geometry.

The export contains 9,888 bark triangles and 123,200 leaf triangles. It places the bottom of the bark at ground level and embeds the bark albedo and a 512 × 512 leaf base-color texture. The exporter bakes the authored Blender color graph, including the veins and UV noise, with a representative value for leaf-to-leaf color variation. The mesh supplies coverage and shading normals, and the viewer applies its own lighting and diffuse backlighting.

### Oak Branch fixed LODs

Building the viewer also builds the offline `BakeFixedLod` tool and generates the branch's `.fixedlod` sidecar and `.fixedlod.txt` report beside its bundled GLB. In **Oak Branch → Branch LOD**, use **Representation** to choose Source GLB or a baked level. **LOD 2 - Locked rims** preserves the outline. **LOD 3 - Rim collapse** permits some outline changes. **LOD 4 - Forced far** allows more rim movement for the far-view comparison. Both rim-collapse levels retain all leaves and their two end anchors. **Color by** shows unlit triangles, fixed groups, or clusters for baked levels. Use **Shaded** for comparisons of lighting and shadow. These controls are session-only.

The baker uses the material's UV set in its collapse cost. For components with usable source UVs, it backs off reductions that would collapse a textured triangle to a line in UV space. The bundled branch uses UV weight 0.1 per coordinate.

**LOD 5 - Four triangles per leaf** and **LOD 6 - Two triangles per leaf** fit the projected area of each original leaf in a reference direction. The two levels share an outline and retain all 3,080 leaves, with 12,320 and 6,160 foliage triangles respectively. LOD 5 keeps a point on the bent centerline. LOD 6 uses a flat pair of triangles.

Choose **Automatic (LOD 3 / 4 / 6)** to let each group switch by projected error. Use **Color by → LOD level** to see the cut, and **Freeze cut** to keep it while you orbit or zoom. **Switch thresholds** starts at 1.70 px for LOD 4 and 1.20 px for LOD 6. **Copy view data** includes the active group level, per-level group counts, and switch estimates alongside the raw errors. **Match camera cut** is the default shadow mode.

Choose **Oak Branch Grid** from the **Scene** menu, or launch `CustomGameEngine.exe --branch-grid`, to explore 25 copies in the benchmark's fixed 5 × 5 layout. It opens with Automatic LOD, indirect draws, and level tints. Orbit with the left mouse button, zoom with the wheel, and use **Distant** to see the whole grid. **Shaded** shows the regular leaf material, and **Freeze cut** holds the selected levels across all copies. The timed route and CSV output are part of the separate benchmark command.

**Indirect draws** batches Automatic foliage by LOD. Use **Freeze cut** to compare drawing paths at a fixed selection. Run `--branch-benchmark report.csv` in a Release build to compare both paths at one and 25 instances. The CSV records CPU selection and submission times, GPU camera and shadow times, and per-pass API call, draw, and triangle counts. [Benchmark instructions](Documentation/OAK_BRANCH_CLUSTER_LOD_PLAN.md#run-the-oak-branch-benchmark)

For a frustum check, open **Oak Branch Grid**, select **Shaded**, enable **Freeze cut**, and pause the wind. Zoom in until the viewport shows part of the grid. Toggle **Frustum culling** under **Branch LOD** and watch **Eligible branches** under **Rendering**. The camera count should drop while the image and visible ground shadows stay steady. The shadow pass tests the full light frustum, which currently encloses the grid. Resume the wind or orbit along the edge of the grid to check branches entering and leaving the camera view.

To compare only caster geometry, freeze the camera cut and pause wind. Use **Shadow LOD** to switch between **Match camera cut** and a forced level. **Off-camera casters only** limits the override to branches outside the camera frustum. **Shadow factor view** shows received shadows in grayscale. Group diagnostics show the estimated error in shadow texels, and **Rendering** shows the number of overridden branches and the shadow foliage workload. Run `--branch-shadow-benchmark report.csv` to repeat the ground and branch shadow comparisons. [Shadow calibration and results](Documentation/OAK_BRANCH_CLUSTER_LOD_PLAN.md#shadow-lod-calibration)

Under **Branch LOD → Voxel comparison**, choose **Source triangles**, **LOD 6 triangles**, or a binary voxel cell size from 0.015625 to 0.5 mesh units. **Camera distance** sets one of the 300–3,000-unit benchmark distances. **Retreat angle**, **Side angle**, and **Rear angle** set the comparison angles. Switch representations at a held camera pose, or orbit and zoom by hand. **Coverage view** gives foliage the same flat color across modes. Shaded voxels use average leaf color and simple lighting. All comparison modes use the rest pose and fixed LOD 6 triangle shadows. **Copy view data** captures the current representation and camera. **Rendering** reports submitted voxel bricks and box triangles. Use `--branch-voxel-benchmark report.csv` to time the camera pass. Append two `.voxels` paths to use custom bakes on the 300–3,000-unit route. [Far-distance comparison](Documentation/OAK_BRANCH_CLUSTER_LOD_PLAN.md#binary-voxel-far-distance-comparison)

Open **Oak Branch Orbit** in the scene picker or launch with `--branch-orbit` to compare **LOD 6 triangles** and the optimized **Voxels 0.125** on the same 30-second orbit. The scene starts paused at 700 units in Coverage view. Wheel over the viewport to zoom, or type an **Orbit distance** and press **Enter**. Play or pause with **B**, restart the lap at the current distance with **Restart orbit**, and drag **Orbit time** to hold a specific pose. Press **R** to reset to the starting pose and 700 units. The route passes the retreat, side, and rear presets. Check **4x MSAA** in Scene controls before comparing.

With **Voxels 0.125** selected, turn off **Coverage view** to compare shared preview lighting with **Baked leaf normals**. The normal bake uses the same binary occupancy and the same shared leaf color. Build the optional `oak_branch_normals` target to regenerate the data and run `--branch-voxel-normal-benchmark report.csv` to time both shading modes on the same mask.

With **Baked leaf normals** enabled, toggle **View-dependent weights** to compare directional weights against pooled weights at a held pose. Append `--directional` to the normal benchmark to time both baked-normal variants. [Directional weights and results](Documentation/OAK_BRANCH_CLUSTER_LOD_PLAN.md#view-dependent-voxel-normal-weights)

Turn on **Unlit base color** and switch between **LOD 6 triangles** and **Voxels 0.125** at a held pose to compare leaf colors directly. Triangles sample their texture and voxels use their shared leaf color. Sunlight and shadows are bypassed for foliage in this view.

Use **Lighting diagnostic → Diffuse and backlighting** to see the direct-light response in grayscale. **Received shadows** shows light visibility, from black for full shadow to white for full light. Run `--branch-lighting-diagnostics report.csv` for paired LOD 6 and normal-shaded voxel captures on the 250- and 700-unit orbits.

**Baked leaf shadow positions** compares sampling the shadow map at a baked point on the leaf surface and at the voxel entry. The toggle is off by default. Append `--shadow-receiver` to the normal benchmark to time both sampling positions. [Shadow receiving results](Documentation/OAK_BRANCH_CLUSTER_LOD_PLAN.md#baked-voxel-shadow-receivers)

Launch with `--branch-transition` or open **Oak Branch Transition** to compare **Automatic branch**, **Automatic regions**, and forced **LOD 6 triangles**. With one branch and **Automatic regions**, click **Find mixed regions**, enable **Tint triangles and voxels**, and use **Freeze cut** to hold the selection while moving closer. In the tint view, triangles are blue and voxels are orange. **25-branch grid** scatters copies in the fixed benchmark pattern. `oak_branch_voxel_regions` regenerates the region ownership data, and `--branch-transition-benchmark report.csv` records matched 4× MSAA captures and timings.

To build the Debug viewer and generate the normal bake in one invocation, use the `$engineCmake` path from the build instructions above.

```powershell
& $engineCmake --build build --config Debug --target CustomGameEngine oak_branch_normals
if ($LASTEXITCODE -eq 0) { .\build\Debug\CustomGameEngine.exe --branch-orbit }
```

Pause wind and compare LOD 4, 5, and 6 from the same pose. Try **Close-up**, **Under canopy**, and **Distant** (300 scene units). Check individual outlines and veins, openings through the branch, and the ground shadow. Use **Copy view data** to record the nearest acceptable pose. **Camera diagnostics** shows orbit distance, eye and focus positions, yaw, pitch, FOV, and viewport. **Group diagnostics** can highlight a group and displays each level's triangle count and projected error estimate for that group. Turn the highlight off for shaded comparisons. [Experiment and area fit](Documentation/OAK_BRANCH_CLUSTER_LOD_PLAN.md#area-fitted-leaf-representations)

The baker can also take a named mesh and write to a separate output for inspection:

```powershell
.\build\Debug\BakeFixedLod.exe .\assets\LowBranchedWhiteOak-SingleBranch.glb 'Oak branch - modeled leaves' .\build\branch-test.fixedlod --triangle-cap 128 --group-size 24 --samples 8 --rim-anchors uv-v --uv-weight 0.1 --leaf-proxies uv-v
```

To regenerate the GLB from the project directory with Blender 5:

```powershell
& 'C:\Program Files\Blender Foundation\Blender 5.0\blender.exe' -b ..\FirstSettlers\ArtSource\WoodlandEdgeHighPoly\LowBranchedWhiteOak-SingleBranch.blend --python-exit-code 1 --python Tools\ExportOakBranch.py -- assets\LowBranchedWhiteOak-SingleBranch.glb
```

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
