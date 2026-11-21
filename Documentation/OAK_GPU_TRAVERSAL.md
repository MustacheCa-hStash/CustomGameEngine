# GPU traversal and draw generation for the forked white oak

The [fixed-cut raster measurements](OAK_FIXED_CUT_RASTER.md) break out visibility, flat color and full materials on these same six cameras.

The camera can consume the existing resident oak cut on the GPU. The CPU supplies the view, tree transforms and whole-tree visibility. Compute propagates visibility through the hierarchy, selects native clusters or reusable spray tiers, bins placements by geometry and emits counted D3D12 indexed draw commands. The command queue consumes those counts directly. A small counter readback after the existing frame fence supplies telemetry.

Two encoders share the selector. The instanced encoder emits one draw for each occupied geometry and material bin. The packed encoder also gathers indices for assembly-space geometry into a resident GPU buffer and issues a single draw for that material's gathered geometry on each tree. Repeated local geometry retains instancing. The full native hierarchy and the reusable canopy with a native trunk run entirely on this camera path. The reusable canopy with the existing trunk sends the trunk through its existing renderer. All D3D arms use the existing shadow path. The CPU live-cut control now selects and submits the correct placements for each visible tree. [Compute passes](../shaders/LiveCut.hlsl) Â· [GPU buffers and submission](../src/TreeGpu.cpp) Â· [Camera integration](../src/D3D12Renderer.cpp)

## Comparison protocol

The bench uses a GeForce RTX 5090, a Ryzen 7 9800X3D and a 3840 Ã— 2160 image with 45Â° vertical FOV. The one-tree case centers the oak. The four-tree case uses the viewer's 2 Ã— 2 grid with approximately 19.017 m spacing, its original rotations and scales, and the same camera target on the central oak. Distances are to that target. More of the grove fits at 41 m than at 24 m. The frustum test is on, HZB occlusion is off, and the static tests freeze wind. Native UE captures use the optimized triangle asset and report zero voxel bricks in all six cases. The voxel-enabled sibling needs its own transition measurement. [Six measured views](../build/oak-gpu-traversal/matched-views.jpg) Â· [UE masks, layouts and FOV](../Tools/CaptureOakGpuComparison.py)

The timed camera has yaw 132Â°. The bench fits each selector on this view and freezes its error multiplier before checking yaws 23Â° and 197Â°. At each pose it measures Nanite's 1 px cut against the 0.001 px native geometry. To qualify, a D3D cut must equal or improve both the occupied-mask IoU and the fraction of outline pixels more than one pixel from their counterpart, against both the UE finest mask and the D3D source mask. The latter uses fixed LOD 4, the 1,504-leaf-triangle and 950-twig-triangle spray supplied to UE. These are whole-image statistics, including gaps within foliage. The one- and four-tree settings are benchmark calibrations for the specified cameras. [Frozen settings](../Tools/OakGpuTraversalSettings.json) Â· [Per-view scores](../build/oak-gpu-traversal/qualified.csv) Â· [CPU and GPU mask comparisons](../build/oak-gpu-traversal/encoder-parity.csv)

D3D timings use three independent 61-frame shaded runs in alternating order. The tree GPU timestamp begins before live selection and ends after the last asset mesh, before ground, sky and display. The traversal-and-encoding timestamp includes the GPU stats copy. Wall time measures the complete render call through GPU completion. PNG readback and encoding happen outside timed frames.

UE also runs a single standalone game viewport at 4K with the project's normal virtual shadow cache, ground and two directional lights. Game-viewport and isolated reference counters are decoded independently from NaniteStats. The tree GPU scope sums Nanite visibility and base pass. Two 61-frame captures follow 100 frames of warm-up per configuration, in forward and reversed order. The report trims eight starting and five ending samples from each UE CSV. The fenced run waits for GPU completion at frame end. The companion run records UE FrameTime without that explicit fence. D3D forward lighting and UE's deferred renderer, virtual shadows, postprocessing and frame scheduling differ. The same-cut D3D CPU/GPU pairing isolates traversal and draw-submission changes. [Standalone game fixture](../Tools/OakAssemblyBench/Source/OakGameTiming/Private/OakGameTiming.cpp) Â· [UE game timings](../build/oak-gpu-traversal/game-profiles.csv) Â· [Isolated UE profiles](../build/oak-gpu-traversal/native-profiles.csv)

## Nominal 1 px selection

The same error rule reproduces native Nanite's single-oak triangle counts exactly. The rotated grove produces 6,757 additional triangles at 24 m, 2,967 at 41 m and exactly the native count at 100 m. The selector uses conservative world-axis bounds on rotated objects. The observed differences occur in the two partially clipped views. Per-cluster attribution remains open. The quality-qualified performance runs use the settings above, which can select finer geometry. [Nominal comparisons](../build/oak-gpu-traversal/nominal.csv)

| Scene | Distance | GPU native cut | UE Nanite | Occupied-mask agreement |
|---|---:|---:|---:|---:|
| One oak | 24 m | 6,516,633 | 6,516,633 | 99.997% |
| One oak | 41 m | 2,513,534 | 2,513,534 | 99.995% |
| One oak | 100 m | 1,073,068 | 1,073,068 | 99.994% |
| Four oaks | 24 m | 11,447,748 | 11,440,991 | 99.947% |
| Four oaks | 41 m | 12,974,581 | 12,971,614 | 99.969% |
| Four oaks | 100 m | 4,590,224 | 4,590,224 | 99.955% |

## Shaded results at matched primary-view quality

Each cell gives submitted camera triangles, shaded tree GPU milliseconds and fenced full-frame wall milliseconds. The CPU and GPU native columns use identical geometry and shaders. CPU native uses its packed assembly draw path. GPU native uses ordinary GPU-generated instanced cluster draws. All six primary views pass both silhouette gates for every column. [All nine D3D arms, full-frame GPU times and run ranges](../build/oak-gpu-traversal/summary.csv) · [Repeated shaded runs](../build/oak-gpu-traversal/timings.csv)

| Oaks | Distance | Shipping grid | CPU native | GPU native | Standalone UE Nanite |
|---:|---:|---:|---:|---:|---:|
| 1 | 24 m | 8,531,783 / 1.952 / 3.725 | 6,929,745 / 1.844 / 4.443 | 6,929,745 / 2.315 / 3.590 | 6,516,633 / 0.769 / 15.216 |

| 1 | 41 m | 4,134,378 / 0.650 / 3.010 | 2,550,956 / 0.416 / 2.791 | 2,550,956 / 0.473 / 1.944 | 2,513,534 / 0.591 / 13.573 |

| 1 | 100 m | 2,470,760 / 0.522 / 2.332 | 1,136,707 / 0.184 / 2.048 | 1,136,707 / 0.238 / 1.601 | 1,073,068 / 0.507 / 15.686 |

| 4 | 24 m | 18,606,782 / 4.676 / 7.475 | 13,039,356 / 2.793 / 10.185 | 13,039,356 / 2.815 / 4.988 | 11,440,991 / 1.002 / 16.200 |

| 4 | 41 m | 20,692,693 / 6.654 / 11.615 | 13,576,967 / 2.978 / 11.375 | 13,576,967 / 3.307 / 5.091 | 12,971,614 / 0.942 / 16.235 |

| 4 | 100 m | 10,491,077 / 4.291 / 6.846 | 4,654,874 / 0.922 / 6.363 | 4,654,874 / 1.014 / 3.564 | 4,590,224 / 0.601 / 18.336 |

GPU native was the fastest complete-frame arm in all three grove cases. Moving that same cut from the CPU to the GPU reduced grove wall time by 51%, 55% and 44% at 24, 41 and 100 m respectively. For one oak the wall savings were 19%, 30% and 22%. The fastest single-oak arms were the packed reusable canopy with a native trunk at 24 m (3.228 ms), the ordinary GPU reusable canopy with a native trunk at 41 m (1.895 ms), and the CPU reusable canopy with the existing trunk at 100 m (1.573 ms). The 24 m winner drew 12.25M triangles. The native cut drew 6.93M there.

On four oaks at 41 m, GPU packing took the native cut from 20,174 indexed draws to 93. The ordinary encoder used 12 CPU-issued indirect submissions. The packed encoder used 24 and took 5.160 ms per frame versus 5.091 ms for ordinary instancing. Across the six configurations ordinary instancing had the lowest median wall time of the two native GPU encoders. At 100 m in the grove, packing did improve tree GPU time from 1.014 to 0.854 ms. At 24 m in the grove it raised measured mesh-pass pixel shader invocations from 12.37M to 13.76M. This comparison supports keeping both encoders and selecting ordinary instancing as the review default. [GPU counters](../build/oak-gpu-traversal/pipeline.csv) · [Encoder times and command counts](../build/oak-gpu-traversal/summary.csv)

The ordinary full native selection and encoding cost 0.041–0.047 ms for one oak and 0.065–0.067 ms for four. In the 41 m grove, 3.307 ms of tree GPU time includes just 0.066 ms in that stage. The native UE tree passes took 0.942 ms. UE sent 104,112 of 104,199 visible clusters through its software rasterizer there. At 100 m it sent all 37,575 grove clusters through software rasterization. Our 41 m matched grove cut carries 4.7% more triangles and takes 3.51 times the tree GPU time. The next triangle performance target is the cost of drawing the selected clusters, including vertex work, overdraw and material shading. An identical cut rendered by each backend with simpler shading would help attribute that residual. [Decoded native counters](../build/oak-gpu-traversal/native-counters.csv) · [UE standalone profiles](../build/oak-gpu-traversal/game-profiles.csv)

The total GPU times for shipping / GPU native / UE were 2.416 / 2.517 / 6.858 ms, 0.856 / 0.679 / 6.343 ms, and 1.180 / 0.442 / 7.867 ms in the one-tree distance sweep. For four oaks they were 5.282 / 4.426 / 7.665 ms, 8.058 / 3.903 / 8.553 ms, and 5.080 / 2.266 / 11.147 ms. UE whole-frame timing varied appreciably between repetitions. For example, at four oaks and 41 m the two fenced walls were 13.22 and 19.25 ms while the two summed Nanite pass measurements were 0.943 and 0.942 ms. Those wall samples include the standalone game's render-thread waits, lighting, virtual shadows and postprocessing. [Per-run native scopes](../build/oak-gpu-traversal/game-profiles.csv)

## Silhouette limits and the voxel handoff

All 90 static GPU/CPU pairings agreed on triangle count and every occupied pixel. All 28 comparisons along the seven-frame wind and orbit sweep also agreed. In the 24 shaded CPU/GPU still comparisons, between 25 and 297 of 8,294,400 pixels differed by more than one 8-bit display code. A silhouette match can coexist with small color differences as draw order changes. [Static parity](../build/oak-gpu-traversal/encoder-parity.csv) · [Moving parity](../build/oak-gpu-traversal/motion-parity.csv) · [Shaded agreement](../build/oak-gpu-traversal/shaded-agreement.csv)

The frozen full native cut passed 15/18 poses. Its three misses were yaw 197° in the grove. Reusable sprays with a native trunk passed 17/18, reusable sprays with the existing trunk passed 16/18 and the stock grid passed 13/18. Two tighter native grove settings passed all three angles each. At 41 m, 0.90 gave 15,442,593 triangles, 3.635 ms tree GPU and 5.407 ms wall at the primary angle. At 100 m, 0.99 gave 4,754,396 triangles, 1.022 ms tree GPU and 3.649 ms wall. The tighter settings were chosen after observing the misses and need fresh angles for further generalization. [Three-angle checks](../build/oak-gpu-traversal/grove-guards.csv) · [Guard timings](../build/oak-gpu-traversal/grove-guard-timing.csv)

The four-oak 24 m holdout remains open. At yaw 197° the two finest reference masks have 99.716% IoU and the Nanite quality gate requires 99.777%. Taking our native selection to 0.001 px produced 17.70M triangles, 99.939% IoU against UE finest, and 99.729% against the D3D source. Tightening from 0.5 px to 0.001 px produced the same count and scores. Investigate the finest asset projections at this angle before spending more triangles there. [Holdout measurements](../build/oak-gpu-traversal/holdout-diagnostics.csv)

**Start the voxel experiment at 100 m.** Use the native GPU cut as the triangle control with the 0.99 grove guard and the measured single-oak setting. Include the reusable canopy with native trunk as a second qualified control. Capture the voxel-enabled UE asset on the same cameras, record where the first bricks appear, and measure silhouette, tree GPU and fenced wall time at that transition. The near-grove reference discrepancy and the multi-tree indexed raster cost remain separate triangle investigations.

## Representation and scaling

The native cut has 316,549 resident nodes and 20,059 geometry bins. The GPU dispatches one level at a time across all trees, rejecting descendants through parent visibility. The native layout takes 15 dispatches including clearing and encoding, at both one and four trees. Ordinary native bins reserve about 12.76 MiB of scratch per tree. Packed native indices add up to 22.23 MiB per tree. The reusable canopy with a native trunk needs about 2.21 MiB plus 11.33 MiB for packing. These are reserved capacities, excluding shared static geometry. A larger forest would benefit from allocating space to the active cut across visible trees.

The installed UE 5.8 shaders expose the perspective edge-scale calculation, persistent node-and-cluster traversal and separate hardware/software raster work generation. Our selector uses the same projected-error decisions for the native data. A depth-wide dispatch still visits the dispatch domain for rejected subtrees, and our draw backend uses the hardware indexed pipeline. The GPU split in the results indicates how much time a replacement traversal could actually recover. [UE edge scales](<C:/Program Files/Epic Games/UE_5.8/Engine/Shaders/Private/Nanite/NaniteCullingCommon.ush>) · [UE cluster traversal and raster arguments](<C:/Program Files/Epic Games/UE_5.8/Engine/Shaders/Private/Nanite/NaniteClusterCulling.usf>)

## Build and view

Run from the engine repository root with the matched oak bakes and UE project from the preceding experiment. The viewer uses the measured error multiplier for the selected layout and distance. Supply `-Trees 1` for the single oak or `-Distance 24` and `-Distance 100` for the other poses. Use `-Yaw 197` to inspect the difficult grove angle. Use `-Arm native-packed` for GPU assembly index packing, `native-cpu` for the CPU packed control, `canopy1-packed` for reusable sprays and a GPU-packed native trunk, or `production` for the shipping grid. [Build script](../Tools/BuildOakGpuTraversal.ps1)

```powershell
.\Tools\BuildOakGpuTraversal.ps1 -Trees 4 -Distance 41 -Arm native-gpu -View
```

`-Benchmark` repeats fitting, validation and profiling. Add `-Recapture` to rebuild the Unreal module and capture both the native masks and the standalone game profiles. [Benchmark driver](../Tools/BenchmarkOakGpuTraversal.py)
