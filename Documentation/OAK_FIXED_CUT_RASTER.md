# White oak raster cost on a fixed triangle cut

The experiment uses the forked white oak, a 3840 × 2160 single-sample camera, yaw 132°, zero wind and one or four trees at 24, 41 and 100 m. The machine has a GeForce RTX 5090, NVIDIA driver 610.88 and a Ryzen 7 9800X3D. Each D3D pose runs at the previously silhouette-qualified error threshold and at Nanite's nominal 1 px threshold. Each UE pose runs at 1 px. [Prior silhouette gates and camera setup](OAK_GPU_TRAVERSAL.md)

## What costs time in the grove

At 41 m the quality-qualified D3D cut submitted 13,576,967 triangles across 20,174 GPU-generated indexed draws, dispatched through 12 ExecuteIndirect calls. Drawing only depth took 1.380 ms, or 67% of the fully shaded replay's 2.065 ms. Adding flat HDR color brought it to 1.406 ms. Switching from flat to the authored forward materials added 0.660 ms. These are differences between complete draws of the same geometry. The ID-writing visibility pass is a separate draw configuration and its 1.764 ms already includes the depth work.

The live renderer took 2.143 ms for the trees, including 0.066 ms to select and encode the cut. Its full GPU frame took 2.732 ms and its fenced wall time was 2.845 ms. The previous live tree measurement was 3.307 ms. The two current frozen full-shader run medians were 2.061 and 2.069 ms. The experiment establishes the cost differences between today's arms, and leaves the historical timing shift unexplained. [Current per-run measurements](../build/oak-raster-profile/d3d.csv) · [Historical runs](../build/oak-gpu-traversal/timings.csv)

Forcing early depth tests on the opaque full shader reduced median pixel shader invocations from 9.69M to 4.32M. The tree time fell from 2.065 to 1.985 ms, a 3.9% saving. All 8,294,400 FP16 HDR pixels matched the normal full shader in every tested pose, threshold and repetition. This flag is exercised only by the capture diagnostic. Production materials with a real alpha cutout need their coverage semantics preserved. [Opaque full shader variant](../shaders/Mesh.hlsl) · [Hardware counters](../build/oak-raster-profile/d3d.csv)

The qualified cuts passed the original fine-reference and source-reference silhouette gates on all six primary cameras. Each D3D mode produced bit-identical depth on each frozen cut. Each UE flat/full pair produced the same hash of the 4K floating-point depth buffer and identical triangle counts. Across engines the 41 m grove's nominal 1 px selections differed by 2,967 triangles and 585 occupied pixels, giving 99.9694% occupancy IoU. At the qualified setting D3D drew 4.67% more triangles than UE and differed by 2,882 occupied pixels. The nominal and qualified comparisons answer different questions. [Direct depth-mask comparison and both gates](../build/oak-raster-profile/quality.csv)

## Six-camera results

All times are tree GPU milliseconds. D3D uses its silhouette-qualified frozen cut. UE uses the 1 px Nanite cut. Its flat and full times sum visibility and the corresponding base pass. The D3D visibility, flat and full columns each time a complete alternative draw. [D3D timings](../build/oak-raster-profile/d3d.csv) · [UE timings and counters](../build/oak-raster-profile/nanite.csv)

| Oaks | m | D3D / UE triangles | D3D depth | D3D visibility | UE visibility | D3D flat | UE flat | D3D full | UE full |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 1 | 24 | 6,929,745 / 6,516,633 | 0.708 | 0.900 | 0.514 | 0.726 | 0.719 | 1.059 | 0.762 |
| 1 | 41 | 2,550,956 / 2,513,534 | 0.257 | 0.349 | 0.418 | 0.266 | 0.563 | 0.411 | 0.586 |
| 1 | 100 | 1,136,707 / 1,073,068 | 0.101 | 0.176 | 0.373 | 0.103 | 0.491 | 0.193 | 0.504 |
| 4 | 24 | 13,039,356 / 11,440,991 | 1.385 | 1.749 | 0.642 | 1.402 | 0.926 | 2.040 | 0.995 |
| 4 | 41 | 13,576,967 / 12,971,614 | 1.380 | 1.764 | 0.649 | 1.406 | 0.880 | 2.065 | 0.936 |
| 4 | 100 | 4,654,874 / 4,590,224 | 0.464 | 0.709 | 0.448 | 0.471 | 0.578 | 0.766 | 0.598 |

At equal nominal error, the four-oak 41 m D3D visibility result was 1.690 ms against UE's 0.649 ms, a factor of 2.60 with just 0.023% more submitted triangles. D3D depth alone took 1.318 ms there. One oak at 41 m showed the other side of the tradeoff. Both engines submitted exactly 2,513,534 triangles and D3D visibility took 0.344 ms against UE's 0.418 ms. The indexed path is already competitive on the smaller workload. [Both error settings](../build/oak-raster-profile/d3d.csv)

### Live full-frame controls

Each cell reports tree GPU / entire GPU frame / fenced frame wall time in ms. The shipping grid and live GPU native cut use their qualified settings. At four oaks and 41 m, the grid submitted 20,692,693 triangles. UE includes deferred lighting, virtual shadows and the rest of its standalone game in its full-frame figures. [Shipping grid runs](../build/oak-raster-profile/grid.csv) · [Live native cut](../build/oak-raster-profile/live.csv) · [Standalone UE](../build/oak-raster-profile/nanite.csv)

| Oaks | m | Shipping grid | Live GPU native cut | UE Nanite |
|---:|---:|---:|---:|---:|
| 1 | 24 | 1.153 / 1.352 / 1.936 | 1.115 / 1.313 / 1.401 | 0.762 / 3.772 / 7.183 |
| 1 | 41 | 0.629 / 0.833 / 1.513 | 0.458 / 0.661 / 0.743 | 0.586 / 3.382 / 6.852 |
| 1 | 100 | 0.515 / 0.720 / 1.115 | 0.234 / 0.437 / 0.516 | 0.504 / 3.271 / 6.671 |
| 4 | 24 | 2.490 / 3.080 / 4.534 | 2.110 / 2.696 / 2.813 | 0.995 / 4.323 / 7.754 |
| 4 | 41 | 3.119 / 3.711 / 6.524 | 2.143 / 2.732 / 2.845 | 0.936 / 4.090 / 7.491 |
| 4 | 100 | 1.954 / 2.556 / 3.582 | 0.839 / 1.439 / 1.534 | 0.598 / 3.425 / 6.876 |

### Next triangle experiment

Prototype a compute visibility pass for the smallest projected triangle clusters, with the existing indexed hardware path handling larger clusters. Use the frozen grove IDs, depth and shaded output as correctness controls. UE routed 104,112 of 104,199 visible clusters through software in the 41 m grove, with zero voxel bricks. Its installed `NaniteRasterizer.usf` implements the cluster software path in `ClusterRasterize` and the hardware mesh-shader path in `HWRasterizeMS`. Begin with the 1 px grove and retain the single oak as a crossover test. The measurements locate a cost in indexed visibility that persists without material shading. Vertex processing, primitive setup and depth traffic still share one timer. [Native cluster counts](../build/oak-raster-profile/nanite.csv)

## What each measurement includes

D3D12 selects the native triangle hierarchy once, then replays the same GPU instance stream, indirect argument and count buffers. Their atomic output order stays fixed within each replay. All five modes share vertex positions, transforms, face culling and depth testing. The tree timestamps exclude shadows, ground, sky, postprocessing, validation readbacks and whole-frame waits. A separate live control selects and emits commands every frame. [Capture code](../src/SceneCapture.cpp) · [Pipelines and timer boundaries](../src/D3D12Renderer.cpp) · [Indirect draws](../src/TreeGpu.cpp)

| D3D mode | Work performed for the selected triangles |
|---|---|
| Depth | Hardware indexed drawing with a D32 depth buffer, no pixel shader and no color attachment. |
| Visibility | Depth plus a 64-bit integer target containing a placement/material key and the primitive index. The vertex stage reads one extra 32-bit per-instance key from device-local memory. |
| Flat | Depth plus one constant color in the normal FP16 HDR scene target. |
| Full | The production forward materials, including leaf response, texture lookups and shadow reception. |
| Early full | The same full shader with early depth/stencil testing forced on these verified opaque materials. |

Each mode gets a separate validation frame with hardware pipeline statistics. The harness checks input-assembler primitive counts, submitted tree counts by material, draw counts, zero traversal dispatches and bitwise equality of every 32-bit depth value. It checks that nonzero visibility IDs cover exactly the depth mask. It also compares the full FP16 HDR frame between ordinary and forced-early shading. Each mode then gets 10 blocks of 24 timed frames after eight warm-up frames per block, in balanced order, with statistics and readbacks disabled. The entire cut is generated again in a fresh process for the second repetition. Tables summarize the two run medians, each calculated from its 10 block medians. The live native-cut and shipping-grid controls use the last 72 of 96 frames in each repetition. [D3D results and block ranges](../build/oak-raster-profile/d3d.csv) · [Live native-cut controls](../build/oak-raster-profile/live.csv) · [Shipping grid](../build/oak-raster-profile/grid.csv)

The standalone UE 5.8 game runs the original Nanite asset and swaps its three material slots. The flat replacements emit constant unlit color, preserve each slot's effective opaque blend and sidedness, and keep separate slots for bark, leaves and twigs. The bark instance overrides its masked parent to opaque. The fixture reads back post-opaque scene depth on a separate validation frame with ground hidden. It restores the ground before capturing Nanite's triangle, brick and software/hardware cluster counters and timings. Each arm gets two fenced runs per camera with 100 warm-up frames and 48 retained samples per run. The visibility figure is the NaniteVisBuffer GPU scope. Flat and full figures use the per-frame sum of NaniteVisBuffer and NaniteBasePass. UE's deferred lighting and other whole-frame work follow separately. Our visibility pass uses fixed-function indexed raster and writes depth plus IDs. Nanite includes selection and hybrid raster in its visibility scope. [UE fixture](../Tools/OakAssemblyBench/Source/OakGameTiming/Private/OakGameTiming.cpp) · [UE per-run results](../build/oak-raster-profile/nanite.csv)

## Reproduce

Run from the engine repository with the staged oak benchmark assets and UE 5.8 installed.

```powershell
.\Tools\BuildOakGpuTraversal.ps1 -RasterProfile
```

To review the qualified four-oak shaded scene interactively, run the following command.

```powershell
.\Tools\BuildOakGpuTraversal.ps1 -View -Trees 4 -Distance 41 -Arm native-gpu
```
