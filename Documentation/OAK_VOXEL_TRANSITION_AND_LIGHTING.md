# White oak voxel transition and grove lighting

At 41 m the four-oak camera saw 0.03% of its tree pixels through Nanite bricks. At 100 m the share was 86%. The 41 m triangle visibility cost can be investigated independently. At 100 m the choice of representation changes both visibility cost and which leaves survive. [Prior fixed-cut raster timings](OAK_FIXED_CUT_RASTER.md) · [Native measurements](../build/oak-voxel-transition/transition.csv)

## Where the camera sees voxels

Both UE 5.8 assets use the same oak input. The experiment compared Voxelize with triangle-only Preserve Area at 3840 × 2160, 45° vertical FOV, nominal 1 px, frustum culling enabled and HZB disabled. The coverage share counts first-visible green pixels in the Voxels visualization divided by all visible oak pixels. The visualization's combined triangle and voxel foreground was checked against an independently captured mask. The pitch is −5.46° through 70 m and −2.235° at 100 m, matching the previous benchmark anchors. The three yaws at 100 m measure view dependence. [Capture and scoring](../Tools/StudyOakVoxelTransition.py)

| Distance | One oak, voxel share | Four oaks, voxel share |
|---:|---:|---:|
| 24 m | 0% | 0% |
| 35 m | 0% | 0% |
| 41 m | 0% | 0.03% |
| 50 m | 0.32% | 4.27% |
| 56 m | 25.62% | 16.97% |
| 58 m | 54.20% | 28.83% |
| 70 m | 82.71% | 54.87% |
| 100 m | 88.56% | 86.32% |

For four oaks at 100 m on the primary camera, Voxelize selected 290,910 surface triangles and 712,001 bricks. Isolated visibility plus base GPU time was 0.573 ms, compared with 0.615 ms for Preserve Area. Against the same 0.001 px finest triangle reference, silhouette IoU fell from 0.9699 to 0.9171. The fraction of outline pixels that missed their counterpart by more than one axial pixel rose from 6.74% to 15.65%. These are medians across two run medians of 52 warm samples, alternating asset order with shadows disabled. The single-oak timings varied more and showed no repeatable 100 m win from enabling voxels. [GPU samples by run](../build/oak-voxel-transition/cost.csv) · [Silhouette measurements](../build/oak-voxel-transition/transition.csv)

### Holding the 100 m silhouette target fixed

We also swept eleven finer Voxelize settings from 0.875 through 0.25 px. For each layout we chose the coarsest tested setting that equalled or beat the 1 px Preserve Area cut on **both** foreground IoU and one-pixel outline error at **all three** camera headings. This gave 0.42 px for one oak and 0.40 px for four. On one oak all three qualified views selected zero bricks. On four oaks only yaw 23° still showed voxels, covering 0.745% of visible tree pixels. On the primary four-oak camera the qualified asset submitted 9,686,746 surface triangles and zero bricks. The threshold spacing bounds what these tests establish. A different bake can compete at the same quality. [Thresholds and all six silhouettes](../build/oak-voxel-transition/match-100-quality.csv) · [Qualified cuts](../build/oak-voxel-transition/match-100-cuts.csv) · [Interleaved timings](../build/oak-voxel-transition/match-100-cost.csv)

The final 100 m timing session ran all three assets/settings together, reversing the order in the second run. The table uses the primary camera. GPU time is the sum of visibility and base pass measured per frame, aggregated as the median of the two run medians.

| Four-oak cut at 100 m | Surface triangles | Bricks | Visible voxel pixels | IoU | Outline miss | GPU ms |
|---|---:|---:|---:|---:|---:|---:|
| Preserve Area, 1 px | 4,590,224 | 0 | 0% | 0.9699 | 6.74% | 0.617 |
| Voxelize, 1 px | 290,910 | 712,001 | 86.32% | 0.9171 | 15.65% | 0.574 |
| Voxelize, 0.40 px | 9,686,746 | 0 | 0% | 0.9747 | 5.87% | 0.675 |

On one oak, Preserve Area took 0.501 ms, nominal Voxelize took 0.517 ms and qualified Voxelize took 0.511 ms. The coarsest qualified Voxelize cut selected 2,077,025 triangles versus 1,073,068 for Preserve Area on that camera. These figures cover isolated tree rendering with shadows off. The capture driver observed a Windows access violation on UE teardown after the capture-complete and log-close messages. It verified every expected fresh artifact, HUD count, visualization mask and timing sample set before reporting. [Timing runs](../build/oak-voxel-transition/match-100-cost.csv) · [Native log](../build/oak-voxel-transition/match-100-profile.log)

## What held back our voxel bake

On exposed bough 7 at 100 m we kept the 3.8 cm cell size, 3,299 leaf bricks, triangle twigs and the original seven training views. We let the fitter consider all 12,520 source-intersecting brick locations, then ranked their coverage across views and refit the active cells. This replaced 365 brick locations. The primary silhouette IoU improved from 0.7809 to 0.8032 and the outline miss rate fell from 12.68% to 12.09%. The strict target for that view is at least 0.8881 IoU and at most 4.10% outline misses. Four of the seven training angles met both native Voxelize thresholds. Zero of the six held-out angles did. Zero of all 13 angles met both Preserve Area thresholds. [Bake and benchmark](../Tools/TestOakVoxelBrickLocations.py) · [Per-angle results](../build/oak-voxel-transition/relocation-quality.csv)

First-visible leaf material IoU rose from 0.730 to 0.754 and twig IoU from 0.462 to 0.466. The p95 first-hit depth error stayed near 2.57 m, versus 1.00 m on the triangle cut. The scoring function can reward filling the correct pixel with foliage far behind the actual visible leaf. All three cuts used the new grove lighting in two alternating-order timing runs. On the isolated bough, 74,047 surface triangles took 0.084 ms of camera GPU time. The relocated cut used 34,384 surface triangles plus 3,299 bricks and took 0.090 ms. Our indexed box rasterizer submits 12 proxy triangles per brick, bringing that cut to 73,972 submitted triangles including proxy geometry. Replacing this bough in the whole crown took 0.577 ms versus 0.567 ms for the all-triangle camera. Neither result supports enabling this bake in the review scene. [First hits](../build/oak-voxel-transition/relocation-first-hit.json) · [Earlier material and depth controls](../build/oak-bough-voxel/depth-material-shadow.csv) · [Paired GPU timings with probes](../build/oak-voxel-transition/relocation-cost.csv)

### Next experiment

The exposed-spray follow-up tested one 12-placement group against default Nanite Voxelize on a moving 50–100 m camera. It qualified a mixed cut and directly compared it with the same coarse twig and leaf triangle tiers. [Group motion, geometry and GPU results](OAK_MIXED_SPRAY_MOTION.md)

1. At the reusable spray group, generate a triangle candidate and a mixed candidate. Start the cell size from group surface area, occupied-cell budget and the child's inherited error. Nanite's group reducer increases cell width by 10% while the candidate can beat triangle simplification error and still needs to fit its budgets. On this bough its selected cells measured 2.07 cm near the transition and roughly 3 cm at 100 m. Retain the option to merge adjacent groups farther out. [Installed group reducer](<C:/Program Files/Epic Games/UE_5.8/Engine/Source/Developer/NaniteBuilder/Private/ClusterDAG.cpp>) · [Decoded native cells and instances](OAK_BOUGH_VOXEL_RESULTS.md)
2. Score depth and first-visible material during fitting. Keep source triangles on the leaf rims and twig tips responsible for repeated outline misses, and let voxels represent foliage behind them. Validate on fresh views and a moving camera, reserving the existing six held-out angles as regression views. Use the same finest-reference IoU and one-pixel edge gates as the triangle control.
3. Measure renderer cost independently by feeding the same decoded native mixed cut into a batched brick visibility pass. Compare it with the current indexed-box path on identical brick masks, depth and materials. The current D3D replay already agrees with Nanite's selected bough mask at 99.34% IoU. This gives a reference for a compute implementation before choosing a new bake. Promote a group when it passes the quality gates and saves shaded GPU time in the crown. [Native replay validation](OAK_BOUGH_VOXEL_RESULTS.md) · [Installed Nanite brick tracer](<C:/Program Files/Epic Games/UE_5.8/Engine/Shaders/Private/Nanite/NaniteRasterizer.usf>)

## Grove lighting

The Forked White Oak and its review launchers now inherit the canopy-filtered diffuse skylight and one ground bounce used in the probe scenes. Mesh leaves use the GLB's authored two-sided diffuse material, its 0.45 transmission factor and its embedded transmission tint texture. The same leaf material shades the underside, using the face orientation and illumination from the opposite hemisphere. Oriented voxel bricks use the same leaf-light response with baked normals and the coarsest transmission-color mip. This preserves average tint and gives the mesh path finer leaf detail. [Scene default](../src/ViewerScene.cpp) · [Mesh illumination](../shaders/Mesh.hlsl) · [Brick illumination](../shaders/Voxel.hlsl) · [Shared response](../shaders/LeafLighting.hlsli)

The 4K checks cover four oaks at 41 m and one oak from 7 m beneath the canopy. In the grove, all lighting variants selected the same 13,576,967 camera triangles and 5,917,512 shadow triangles. Canopy filtering changed 96.8% of visible tree pixels by more than one display-code value. Enabling the bounce changed 89.0% relative to filtered sky alone. The transmission-only render lit 87.8% of visible leaf pixels in the grove and 89.1% beneath the canopy. Setting the factor to zero produced black in both. On the isolated voxel bough, 8,430 of 13,140 leaf pixels transmitted measurable light, and the zero-factor control was black on all leaf pixels. The four-oak tree pass measured 2.16 ms with probes off and 2.39 ms with bounce in this lighting check, one run per mode. [Side-by-side captures](../build/oak-grove-lighting/comparison.png) · [Fixed-geometry checks](../build/oak-grove-lighting/checks.csv) · [Brick lighting check](../build/oak-voxel-transition/brick-lighting.json)

## Build and reproduce

Run from the engine checkout. The first command builds Release and opens the lit grove on the reviewed GPU cut. The scripts use the existing staged UE benchmark and bough bake fixtures.

```powershell
.\Tools\BuildOakGpuTraversal.ps1 -View -Trees 4 -Distance 41
```

To reproduce the experiments with the benchmark's NumPy/Pillow Python environment, close the interactive viewer first.

```powershell
$benchPython = Join-Path $env:USERPROFILE '.cache/codex-runtimes/codex-primary-runtime/dependencies/python/python.exe'
& $benchPython -B Tools/StudyOakVoxelTransition.py
& $benchPython -B Tools/StudyOakVoxelTransition.py --match-100
& $benchPython -B Tools/TestOakVoxelBrickLocations.py
& $benchPython -B Tools/CheckOakGroveLighting.py
```

To regenerate the prerequisite exposed-bough samples and controls, use `.\Tools\BuildOakBoughVoxels.ps1 -Refit -Benchmark` with the existing bough fixtures. Historical triangle benchmarks explicitly select open sky so they can be compared with previous runs.
