# Reusable white oak spray cuts at 41 and 100 m

Five reusable leaf-and-twig meshes cover 705 placements on two boughs. At the 132° benchmark camera they save 44–68% of the isolated grid triangles. Replacing just those two regions saves 238,093 camera triangles at 41 m and 190,663 at 100 m versus the complete production tree, with each side calibrated to the crown silhouette. Native Nanite still draws fewer triangles in all four isolated cases. [Measured recipe](../Tools/ReusableOakRecipe.json) · [Crown measurements](../build/oak-assembly-step/reusable/crown-chosen.json)

## What was compared

The shared input is a 2,454-triangle spray with 1,504 leaf and 950 twig triangles. The exposed bough 7 has 371 sprays, and interior bough 8 has 334. Ranked by frontmost source pixels among boughs with 200–450 placements, bough 8 has the lowest mean exposure across the two benchmark distances. The rest of the tree leaves 43.7% and 45.2% of bough 7's isolated silhouette visible at 41 and 100 m, versus 1.5% and 1.8% of bough 8's. Testing both in isolation prevents the hidden crown interior from concealing simplification damage. [Exposure by bough](../build/oak-assembly-step/reusable/exposure.csv) · [Source membership](../Tools/ReusableOakSprays.py)

The RTX 5090 cameras use 3840 × 2160, 45° vertical FOV, 1× sampling, frozen wind, frustum culling and no HZB. For each isolated bough, each renderer compares its selected mask against its own finest mask at the same pose. Passing requires at least Nanite's occupied-pixel intersection over union (IoU) and at most its fraction of outline pixels farther than one pixel from a matching edge, counting both directions and holes. Isolated D3D draws use two-sided leaves and one-sided wood to match UE. The crown comparison uses the existing production material settings and crown reference. An isolated stock control uses the current grid baker restricted to exactly the same bough membership. Stock gets one quality setting per bough and distance across all seven qualification angles. The pilot uses one shared recipe and one template error setting throughout. [Grid measurements](../build/oak-assembly-step/reusable/grid-chosen.json) · [Mask measurements](../build/oak-assembly-step/reusable/pilot-masks.csv) · [UE crown thresholds](../build/oak-gpu-selection/pixel-edge-audit.csv)

## GPU measurements

| Isolated bough | Current grid triangles / ms | Reusable triangles / ms | Native Nanite triangles / ms | Nanite geometry in D3D, ms |
|---|---:|---:|---:|---:|
| Exposed, 41 m | 337,621 / 0.1131 | 183,040 / 0.1001 | 158,190 / 0.432–0.435 | 0.0915 |
| Exposed, 100 m | 199,472 / 0.0987 | 74,047 / 0.0791 | 58,823 / 0.413–0.442 | 0.0750 |
| Interior, 41 m | 209,168 / 0.1072 | 118,086 / 0.0937 | 104,766 / 0.455–0.458 | 0.0854 |
| Interior, 100 m | 144,122 / 0.0932 | 45,758 / 0.0765 | 42,272 / 0.420–0.449 | 0.0747 |

At 132°, reusable versus native IoU is 92.22% versus 91.53% (exposed, 41 m), 90.30% versus 88.81% (exposed, 100 m), 91.76% versus 91.00% (interior, 41 m) and 89.35% versus 87.39% (interior, 100 m). Corresponding reusable / native outline misses are 2.86% / 3.11%, 3.49% / 4.10%, 3.58% / 3.81% and 3.64% / 4.02%. [Pose-level masks and counts](../build/oak-assembly-step/reusable/pilot-masks.csv)

Each D3D GPU time is the median of four 61-frame medians, alternating the order of runs. Timings include the shaded camera, ground and sky. The five degenerate triangles that suppress unrelated regions in the isolated viewers are excluded from the reported tree counts. UE's two shaded runs each supply a median of Nanite visibility plus Nanite base-pass GPU time after trimming the first eight and last five samples. For a common-pass comparison, we also drew the actual UE-selected triangles through D3D with the same materials and sidedness. Their masks agree with native UE at 99.964–99.987% IoU. That replay measures drawing the selected geometry. Native UE also performs its own traversal and rasterization. [Matched costs](../build/oak-assembly-step/reusable/matched-costs.csv) · [D3D runs](../build/oak-assembly-step/reusable/timings.csv) · [Exact replay checks](../build/oak-assembly-step/reusable/guard-finalists.log) · [Native UE profiles](../build/unreal-nanite-oak/Saved/Profiling/CSV)

| Complete crown | 41 m triangles | 41 m GPU ms | 100 m triangles | 100 m GPU ms |
|---|---:|---:|---:|---:|
| Production | 3,654,645 | 0.6950 | 2,433,509 | 0.5725 |
| Stock grid with both bough boundaries | 3,608,909 | 0.6883 | 2,457,836 | 0.5736 |
| Reusable on both boughs | **3,416,552** | **0.6618** | **2,242,846** | **0.5312** |
| Native Nanite | 2,513,534 | 0.562–0.563 | 1,073,068 | 0.480–0.517 |

The reusable crown passes at 97.108% IoU at 41 m and 95.900% at 100 m. It still submits 35.9% and 109.0% more triangles than native UE. Whole-crown UE counts come from the existing shared-asset benchmark. The four isolated UE counts come directly from decoded GPU buffers. All UE times in this table are fresh shaded profiles on the same benchmark machine. [Whole-tree benchmark](OAK_ASSEMBLY_41_100.md) · [Native UE 41 m crown profile](../build/unreal-nanite-oak/Saved/Profiling/CSV/spray-final-crown-41-r1.csv)

At a common stock budget, changing only the two regions saves **163,122 triangles at 41 m** (1.02429 px) and **193,369 at 100 m** (0.48830 px) versus the same-boundary grid. Each mode in the crown table uses its own cheapest passing cut found in the sweep. The rest of the crown can change when those budgets differ. The production, grid and pilot crown all clear the established benchmark thresholds at 132°. Crown qualification covers that pose. [Matched and fixed-budget settings](../build/oak-assembly-step/reusable/crown-chosen.json)

## Where the remaining triangles go

| Bough and distance | Pilot leaf / twig triangles | Native Nanite leaf / twig triangles | Pilot / Nanite p95 first-hit depth difference from source |
|---|---:|---:|---:|
| Exposed, 41 m | 125,027 / 58,013 | 89,112 / 69,078 | 524 / 617 mm |
| Exposed, 100 m | 39,663 / 34,384 | 34,161 / 24,662 | 992 / 1,067 mm |
| Interior, 41 m | 75,668 / 42,418 | 52,022 / 52,744 | 747 / 793 mm |
| Interior, 100 m | 31,062 / 14,696 | 28,131 / 14,141 | 925 / 1,028 mm |

At 41 m the extra geometry is in the leaves. Both boughs spend fewer twig triangles than Nanite, and their twig-only IoU falls below Nanite's at this pose despite passing the combined gate. On the exposed bough at 100 m, twigs account for 9,722 of the 15,224 extra triangles. On shared foreground, the pilot has lower median and p95 first-hit depth error in all four primary views. Thin foreground branches sometimes uncover a surface roughly a metre behind them. [Per-material coverage and depth](../build/oak-assembly-step/reusable/depth-materials.csv)

The native readback makes the selection gap concrete. At 41 m on bough 7, Nanite draws 307 part instances at 378 triangles and 63 at 630. At 100 m, 271 use 125 and 92 use 252. Bough 8 at 100 m contains 320 instances at 125 triangles, plus assembled geometry. Across these views, 95–98% of Nanite's triangles come from shared part transforms. Our 100 m interior cut uses 137 triangles for each of its 334 placements. On the exposed bough it selects 137 for 156 sprays and 245 for 215. We move too many sprays to the expensive level there. The native camera used triangle clusters in all four captures. [Exposed readback](../build/oak-assembly-step/reusable/live7-100.clusters.json) · [Interior readback](../build/oak-assembly-step/reusable/live8-100.clusters.json) · [Pilot selections](../build/oak-assembly-step/reusable/selection.csv)

## Bake and runtime

The offline Unreal module uses Epic's graph partitioner, quadric simplifier and area preservation, and compares meshoptimizer on the same groups. Each final mesh draws two material ranges. The packager shares identical ranges between tiers and stores placement references. The viewer selects a tier from a conservative camera projection bound per spatial group of up to 12 sprays and issues indexed instanced draws. The groups and policy are shared by the leaf and twig slots. The five available meshes contain 137, 245, 309, 464 and 512 triangles. The fitted projection parameters are empirical. The largest local projection scale in the seven calibration cameras was 80.09 pixels per world metre. The finest simplified tier ends at 81. Larger groups in the trial subtree draw the shared 2,454-triangle source spray. [Offline module](../Tools/OakAssemblyBench/Source/OakAssemblyBench/Private/OakAssemblyBench.cpp) · [Epic cluster DAG](../../UnrealEngine/Engine/Source/Developer/NaniteBuilder/Private/ClusterDAG.cpp) · [Epic foliage area preservation](https://dev.epicgames.com/documentation/unreal-engine/working-with-naniteenabled-content) · [Packaging](../Tools/PackageReusableOak.py)

The pilot sidecar adds **258,378 bytes** to the 155,645,596-byte same-boundary grid. Each of the two instanced materials also uploads a reordered GPU transform array (4,730 × 40 bytes per material, before allocation alignment). Close cameras route back to the existing regional grid. At 24 m the two versions produced identical masks and camera triangle counts. Shadow traversal used the grid and submitted the same 1,479,378 triangles in all three distance checks. Receiver appearance and animated wind remain unmeasured. The new sidecar uses CPU selection, with direct or indirect draw submission. The benchmark uses direct submission. At the 100 m crown pose, indirect submission and the two GPU-mode fallback requests produced the exact direct mask and tree count. [Fallback checks](../build/oak-assembly-step/reusable/close-shadow-check.csv) · [Runtime selection](../src/D3D12Renderer.cpp) · [GPU mode handling](../src/TreeGpu.cpp) · [Submission checks](../build/oak-assembly-step/reusable/submission-check.csv)

## Step 4

The next experiment measures native voxel cell sizes and tests an exposed bough at 100 m. [Experiment plan and acceptance criteria](OAK_BOUGH_VOXEL_EXPERIMENT.md)

The production grid and final reusable recipe pass both quality gates on **all 44 isolated comparisons** (two boughs, two distances and 11 azimuths). Seven azimuths informed the template fit. After freezing it, we tested 68° and 315°. At 315° the interior 41 m cut missed both gates with 171,008 triangles and groups projecting up to 83.07 pixels/metre. We limited simplification to its measured projection range. The resulting cut spends 264,224 triangles, versus 322,255 on the stock grid, and reaches 93.36% IoU / 2.75% outline misses against Nanite's 92.37% / 3.10%. We then tested 302° and 330° without further changes. Both boughs pass at both distances there. The projection fallback never activates in the four primary comparisons. [Initial miss](../build/oak-assembly-step/reusable/pre-guard-validation.csv) · [Additional views with the final cut](../build/oak-assembly-step/reusable/frozen-validation.csv)

**Proceed to the one-bough voxel experiment at 100 m.** Use exposed bough 7 as the first comparison. It has the largest remaining triangle gap (25.9%), the outline is visible on the complete tree, and there is now a measured triangle control. Require the same isolated and crown silhouette gates, compare the GPU camera pass against both the reusable cut and the replay of Nanite geometry, and record caster/receiver quality, depth and transition frames. Report occupied bricks and their real GPU cost alongside submitted triangles. For the triangle path, the next priorities are a cheaper leaf cut at 41 m and finer-than-12-spray choices for the exposed twigs at 100 m. Both should be tested on more boughs before enabling the pilot for production.

## Follow-up on triangle coverage before voxelization

The voxel-enabled Nanite build of the **same optimized spray** still draws zero bricks at 14, 18, 24 and 41 m. This follow-up used the 4K quarter-view benchmark, 45° vertical FOV, 1 px native Nanite setting, frustum culling and HZB disabled. At every sampled pose through 44 m, all visible oak pixels were triangles. At 48 m, 87 of 355,127 visible oak pixels were voxels (0.0245%). The share passed 1% between 50 and 52 m, passed 50% between 56 and 58 m, and reached 88.56% at 100 m. The original high-poly spray passed 1% earlier, between 28 and 30 m. These distances apply to these assets and this camera. Longer streaming holds on the return to 24 and 41 m reproduced the voxel shares. [Native representation sweep](../build/unreal-nanite-oak/triangle-coverage-check/representation.csv) · [Offscreen triangle and brick counters](../build/unreal-nanite-oak/triangle-coverage-check/triangle-counters.png)

| Camera | Current two-bough pilot, whole tree | Native voxel-enabled tree | Custom triangle excess |
|---|---:|---:|---:|
| 14 m | 11,816,359 | 11,358,109 | +4.0% |
| 18 m | 10,359,243 | 10,416,784 | −0.6% |
| 24 m | 8,020,905 | 6,527,470 | +22.9% |
| 41 m | 2,702,236 | 2,138,987 | +26.3% |

Each row uses the cheapest sampled custom cut meeting the voxel-enabled asset's own occupied-pixel IoU and one-pixel edge targets. The custom global setting was calibrated per distance. Each engine uses its own finest mask, with 99.66–99.68% overlap between the two finest references. At 41 m the custom and UE results are 96.37% and 96.36% IoU, with 5.99% and 6.38% outline misses. The earlier 3.417M versus 2.514M comparison used the stricter Preserve Area silhouette from that build. Both comparisons support the same priority: close the whole-crown triangle gap at 24–41 m. The 100 m one-bough voxel experiment tests the farther representation. [Final comparisons](../build/unreal-nanite-oak/triangle-coverage-check/pre-voxel-comparison.csv) · [Cut sweep](../build/unreal-nanite-oak/triangle-coverage-check/quality-sweep.csv)

## Build and view

Run from the repository root with the existing shared-input benchmark assets. The default command builds Release and packages the measured templates. Add `-Rebake` to rebuild the regional grid and rerun all selected reductions with the installed UE 5.8 editor. The recipe verifies the placement lists and the SHA-256 digests of every selected mesh and material range. [Build script](../Tools/BuildReusableOakSprays.ps1)

```powershell
.\Tools\BuildReusableOakSprays.ps1
$dir = '.\build\oak-assembly-step\reusable'
$view = '--review-capture','--scene','forked-oak','--camera','overview','--width','3840','--height','2160','--yaw-degrees','132','--msaa','1','--wind-strength','0','--wind-time','0','--lod','auto','--spray-mode','hierarchy','--tree-submit','direct','--spray-culling','on','--spray-local-lod','on','--lighting','combined','--probe-gi','off'
& "$dir\pilot\CustomGameEngine.exe" @view --distance 41 --pitch-degrees -5.46 --spray-pixel-error 1.03649
& "$dir\pilot\CustomGameEngine.exe" @view --distance 100 --pitch-degrees -2.235 --spray-pixel-error 0.49018
```

To see a bough in isolation, substitute `pilot-7` or `pilot-8` for `pilot`, use `--spray-pixel-error 1` and add `--cull-spray-wood-backfaces`. The default template budget is 1 px. The full rebake reproduced both the regional grid and the packaged crown byte for byte, and both registered CTest fixtures pass. [Silhouette comparison image](../build/oak-assembly-step/reusable/matched-boughs.png) · [Build log](../build/oak-assembly-step/reusable/guard-rebuild.log) · [Measured and rebuilt digests](../build/oak-assembly-step/reusable/rebake-sha256.json) · [CTest](../build/oak-assembly-step/reusable/ctest.log)
