# White oak bough 7 with cross-spray triangle reduction

The follow-up tested reusable leaf-and-wood cuts on both the exposed and interior bough. [Two-bough results and viewer command](OAK_REUSABLE_SPRAYS_41_100.md)

**The wood pilot beats Nanite's submitted triangle count at both distances and passes its exposed-silhouette thresholds at five yaws.** The complete tree passes the crown thresholds at the benchmark yaw. The larger remaining opportunity is a shared leaf-and-wood spray with several selectable cluster replacements. The two-material bake still spends more triangles than Nanite. Its GPU readback shows exactly which material consumes them.

## Exposed wood

The same 371 placements use 950 wood triangles per source spray. Tests use the 4K RTX 5090 benchmark, 45° vertical field of view, 1× sampling, fixed wind, frustum culling and no HZB. Each implementation compares against its own finest silhouette. A passing cut must equal or exceed UE's occupied-pixel intersection over union and have no more outline pixels beyond one pixel in either direction, including internal gaps. The five yaws are 132°, 90°, 180°, 45° and 225°. The last two were rendered in both engines after we chose the cuts. An automatic cut was checked against its frozen geometry at the original three yaws. At 24 m and 1 px the pilot fell back to the existing hierarchy and rendered the same mask and wood count as the grid. [Wood material validation](../build/oak-assembly-step/one-face.csv) · [Additional cameras](../build/oak-assembly-step/additional-views.csv) · [Automatic traversal](../build/oak-assembly-step/auto-selection.csv)

| Wood camera | Previous grid triangles / ms | Previous connected triangles / ms | Assembly triangles / ms | Native Nanite triangles / ms |
|---|---:|---:|---:|---:|
| 41 m | 191,856 / 0.0868 | 140,137 / 0.0839 | **87,912 / 0.0821** | 93,820 / 0.433–0.434 |
| 100 m | 67,994 / 0.0719 | 68,546 / 0.0725 | **21,329 / 0.0722** | 24,424 / 0.381–0.406 |

The two prior local controls use their previously calibrated settings and fresh GPU timings. Counts exclude the degenerate triangles used to hide unrelated draws in those viewers. Our times here use the stock two-sided shaded camera, including sky and ground. Each is the median of four 61-frame medians in alternating order. Native UE times cover Nanite visibility plus Nanite base pass. The two engines' GPU columns have different pass boundaries. The closest comparison of geometry cost draws the decoded Nanite cut through the same D3D camera, using the same back-face policy. [Alternating D3D runs](../build/oak-assembly-step/final-timings.csv) · [Native UE wood captures and profiles](OAK_BRANCH_CUT_41_100.md)

Under that same-camera replay, the new wood costs **0.0786 ms versus 0.0769 ms** for the actual Nanite geometry at 41 m, and **0.0717 ms versus 0.0718 ms** at 100 m. The 41 m triangle saving buys no GPU-time advantage here. These timings use back-face culling on camera meshes with the ordinary sky pipeline. [Same-renderer timings](../build/oak-assembly-step/final-timings.csv)

At the benchmark yaw, the new wood with UE's material-sidedness rule reaches **90.82% IoU / 2.44% outline misses** at 41 m and **51.80% / 15.47%** at 100 m. Native UE reaches **82.30% / 2.95%** and **46.55% / 16.25%**. The stock two-sided camera also clears both gates at each of the original three yaws. Measured against the same source on shared foreground pixels, benchmark p95 first-hit depth error is **1.17 mm versus Nanite's 4.83 mm** at 41 m and **0.657 m versus 1.144 m** at 100 m. All three original yaws favor the selected wood in p95. Thin foreground twigs can expose another branch a metre behind them at 100 m. A matched silhouette still allows that depth change. [Silhouette overlays](../build/oak-assembly-step/wood-silhouettes.png) · [Depth, common-pixel counts and tails](../build/oak-assembly-step/foreground-depth.csv) · [Stock camera masks](../build/oak-assembly-step/final-masks.csv)

### How the bake reached these cuts

- At **41 m**, meshoptimizer first reduced the reusable 950-triangle spray to 474 triangles. Epic's simplifier then reduced groups across placements to 87,912 triangles. Starting from all expanded input triangles and using Epic at both levels also passed with 88,077 triangles, but p95 depth error rose to 2.69 mm. On the same final expanded groups, meshoptimizer produced 87,278 triangles and missed the edge gate.
- At **100 m**, Epic produced the intermediate reductions, and meshoptimizer reduced the final groups to 21,329 triangles. The all-Epic arm at that reduction ratio produced 22,011 triangles and missed a silhouette gate. Recursively using meshoptimizer produced 21,163 triangles and passed only one of the original three yaws in the stock camera. A different Epic reduction ratio also passed with 23,647 triangles. Starting from a reusable reduced spray gave another passing 21,345-triangle result, with higher benchmark p95 depth error. [Paired reducers](../build/oak-assembly-step/flat-validation.csv) · [Reusable part trials](../build/oak-assembly-step/seed-score.csv)
- The offline editor module compiles the installed engine's graph partitioner, calls Epic's `FMeshSimplifier` and `PreserveSurfaceArea`, and runs the repository's meshoptimizer on identical group inputs. It partitions into clusters of up to 128 triangles, groups up to 32 clusters by shared edges and locality, locks external edge positions, reduces pooled geometry and partitions again. The runtime consumes ordinary tree hierarchy data. The pilot stores two added whole-bough choices. Nanite can make finer local choices and reuse selected clusters across instances. [Offline module](../Tools/OakAssemblyBench/Source/OakAssemblyBench/Private/OakAssemblyBench.cpp) · [Local UE cluster DAG](../../UnrealEngine/Engine/Source/Developer/NaniteBuilder/Private/ClusterDAG.cpp) · [Local UE simplification](../../UnrealEngine/Engine/Source/Developer/NaniteBuilder/Private/Cluster.cpp)

## Whole crown and production packing

All custom rows below use exactly 1.02211 px at 41 m and 0.49018 px at 100 m. UE uses 1 px. These were the existing benchmark settings. The remainder of the crown uses the same shared-input assets. All rows pass the benchmark crown gates.

| Camera workload | 41 m triangles | 41 m GPU ms | 100 m triangles | 100 m GPU ms |
|---|---:|---:|---:|---:|
| Production before first-use packing | 3,671,049 | 0.762 | 2,433,509 | 0.663 |
| Production with first-use packing | 3,671,049 | 0.702 | 2,433,509 | 0.572 |
| Same bough boundary, first-use grid | 3,664,867 | 0.673 | 2,433,581 | 0.516 |
| Same boundary and packing, assembly pilot | **3,653,722** | **0.673** | **2,341,288** | **0.508** |
| Native Nanite | 2,513,534 | 0.562–0.564 | 1,073,068 | 0.478–0.519 |

Changing only the wood replacement at this boundary saves **11,145 triangles at 41 m** and **92,293 at 100 m**. The 41 m GPU medians are effectively tied. The 100 m difference is about 0.008 ms. First-use packing alone saves **0.060 and 0.091 ms** on production's existing geometry. The production check compared the unchanged attribute bytes at every referenced index, all nodes and the other part buffers. The 1,131,297 shared wood vertices changed order and the selected triangles and camera masks stayed identical. [Production packing](../Tools/BakeSprayBough.cpp) · [GPU runs](../build/oak-assembly-step/final-timings.csv) · [Crown masks](../build/oak-assembly-step/final-masks.csv)

The pilot crown reaches **97.157% / 4.794%** and **95.893% / 6.106%** for IoU / edges beyond one pixel. At 100 m the IoU gate is 95.889%, a narrow margin. The pilot still spends 45.4% and 118.2% more total triangles than Nanite. Leaves and trunk alone consume 982,544 triangles at the common 100 m setting. Production now uses first-use wood packing. The two empirical projection thresholds belong to the opt-in shared-input pilot. Wind, receiver shadow quality, a moving camera crossing the thresholds and generalization to other boughs remain open. The pilot sidecar takes about 200 MiB, versus 196 MiB for the same-boundary first-use grid and 148 MiB for production. [Projection and fallback](../Tools/PackageOakAssembly.py) · [UE crown targets](../build/oak-gpu-selection/pixel-edge-audit.csv)

## What Nanite actually selected

The GPU readback contains the camera's visible clusters, indirect raster counts, cluster pages and assembly transforms. Decoding the triangle indices, positions, normals, UVs, material ranges and transforms produced these counts. The twig material is one-sided and the leaf material is two-sided. Replaying the wood cut through D3D with hardware back-face culling differs from the actual UE mask by **6 extra / 4 missing pixels at 41 m** and **2 extra / 1 missing at 100 m**. The two-material replay differs by 8 / 4 and 3 / 2 pixels. This agreement is for the exact live cut. UE's finest wood raster still differs from our shared input by about 5% of occupied pixels, so each engine's finest geometry supplies its simplification reference. [Readback decoder](../Tools/DecodeNaniteOakCut.py) · [Wood raster controls](../build/oak-assembly-step/raster-control.csv) · [Two-material controls](../build/oak-assembly-step/joint-readback-control.csv)

| Live UE workload | Selected clusters | Unique page/cluster pairs | Part transforms used | Leaves | Wood |
|---|---:|---:|---:|---:|---:|
| Wood only, 41 m | 748 | 10 | 370 | — | 93,820 |
| Wood only, 100 m | 194 | 194 | 0 | — | 24,424 |
| Leaves and wood, 41 m | 1,271 | 43 | 370 | 89,112 | 69,078 |
| Leaves and wood, 100 m | 471 | 19 | 363 | 34,161 | 24,662 |

**Adding leaves changes which hierarchy Nanite actually uses.** At 100 m, all wood-only triangles came from assembled geometry. On the combined bough, 57,059 of 58,823 triangles came through shared part transforms. At 41 m the wood-only capture reuses just ten distinct clusters across the entire draw. Attributing the full two-material advantage to cross-spray merging would miss this behavior. [Wood cluster trace, 41 m](../build/oak-assembly-step/nanite41.clusters.json) · [Wood cluster trace, 100 m](../build/oak-assembly-step/nanite100.clusters.json) · [Combined trace, 41 m](../build/oak-assembly-step/joint-nanite41.clusters.json) · [Combined trace, 100 m](../build/oak-assembly-step/joint-nanite100.clusters.json)

## Leaves and wood together

| Bough with both materials | 41 m triangles / ms | 100 m triangles / ms |
|---|---:|---:|
| Separate leaf and wood cuts | **227,612 / 0.1060** | **69,019 / 0.0783** |
| Shared reduction groups with material IDs | 227,665 / 0.1050 | 112,939 / 0.0815 |
| Native Nanite | **158,190 / 0.429–0.462** | **58,823 / 0.444–0.445** |

All three rows meet the combined silhouette criterion at 132°, 90° and 180°. For mixed sidedness we take the union of the two actual D3D coverage masks. GPU timings use a single ordinary shaded draw of the two-material bough. Independent first-hit comparisons use a perspective-correct depth raster. The 41 m separate cut spends 139,535 triangles on leaves and 88,077 on wood. At 100 m it spends 34,895 and 34,124 respectively. A cheaper 56,224-triangle combination passes the main 100 m pose but misses the edge and occupancy targets at 90°. The alternatives near Nanite's allocation likewise miss at least one angle. [Material sweep](../build/oak-assembly-step/joint-sweep.csv) · [D3D mixed-material masks](../build/oak-assembly-step/joint-hardware.csv) · [Per-material counters, silhouette and depth](../build/oak-assembly-step/material-audit.csv) · [Native joint profile](../build/unreal-nanite-oak/Saved/Profiling/CSV/assembly-joint-100-r1.csv)

**Keep the production packing. Advance the triangle work before step 4's voxel bake.** First bake reusable two-material spray cluster alternatives and let the selector choose among them per local group. Target roughly 89k leaf triangles at 41 m and 25k wood triangles at 100 m on this bough. Keep the measured leaf, wood, combined silhouette and foreground-depth scores as independent signals. Then repeat on additional exposed and interior boughs, check moving wind and caster/receiver shadows, and measure memory before expanding across the whole tree. The current bake stores whole-bough alternatives. The readbacks directly support testing substantially finer shared-part choices.

## Build and inspect

From the repository root, PowerShell can build the Release viewer and package the measured cuts from the existing shared-input benchmark. The script accepts `-Python` for a Python executable with NumPy and Pillow. To run the offline reducer workflow with the installed UE editor, pass `-Rebake`. Jobs reuse their outputs when the input, material map, reducer settings and compiled module hashes match. Rebuilding both selected meshes reproduced their measured SHA-256 digests byte for byte. Both build paths use the existing benchmark's grid-control asset directory and shared spray/camera fixture. [Full rebuild log](../build/oak-assembly-step/rebuild-check.log)

```powershell
.\Tools\BuildOakAssemblyPilot.ps1
$viewer = '.\build\oak-assembly-step\assembly-pilot\CustomGameEngine.exe'
$common = '--review-capture','--scene','forked-oak','--yaw-degrees','132','--msaa','1','--wind-strength','0','--lod','auto','--spray-mode','hierarchy','--tree-submit','direct','--lighting','combined','--probe-gi','off'
& $viewer @common --distance 100 --pitch-degrees -2.235 --spray-pixel-error 0.49018
```

For 41 m, use `--distance 41 --pitch-degrees -5.46 --spray-pixel-error 1.02211`. The `production-new` and `packed-grid` directories beside `assembly-pilot` contain the matched controls. The isolated `selected-41` and `selected-100` viewers show the wood alone. Add `--cull-mesh-backfaces` for the one-sided isolated view. [Build script](../Tools/BuildOakAssemblyPilot.ps1) · [Release build](../build/oak-assembly-step/final-build.log) · [CTest](../build/oak-assembly-step/ctest.log)
