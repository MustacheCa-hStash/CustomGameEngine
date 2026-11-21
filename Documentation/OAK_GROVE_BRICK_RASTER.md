# Four white oaks in motion with a mixed canopy cut

The brick cut met native Nanite’s default silhouette quality in every captured frame. The current GPU triangle renderer was faster across both camera ranges. Replacing one bough per oak reduced the reusable triangle hierarchy’s surface count by 265,084 triangles at 100 m and 201,796 at 175 m. Both replacements cost 47,068 bricks.

## Grove measurements

Each cell lists median camera surface triangles in millions, median submitted 4³ bricks in thousands, and tree GPU milliseconds. Work counts are per frame. GPU times are medians of run medians. A median brick count can lie between two actual instance counts.

| Whole four-oak grove | 50–100 m, M tris / k bricks / ms | 100–175 m, M tris / k bricks / ms | Quality passes out of 104 |
|---|---:|---:|---:|
| Reusable triangle hierarchy, unchanged cut policy | 6.935 / 0 / 1.290 | 5.594 / 0 / 0.938 | 104 |
| Same hierarchy with the batched brick rasterizer | 6.799 / 17.7 / 1.366 | 5.392 / 47.1 / 0.957 | 104 |
| Same mixed cut using hardware boxes | 6.799 / 17.7 / 1.307 | 5.392 / 47.1 / 0.953 | 104 |
| Current GPU triangle renderer, 1 px | 7.321 / 0 / 1.202 | 1.602 / 0 / 0.430 | 100 |
| Current GPU triangle renderer, 0.90 px | 7.321 / 0 / 1.202 | 2.269 / 0 / 0.545 | 104 |
| Native Nanite Preserve Area, 1 px | 7.321 / 0 / 0.673 | 1.602 / 0 / 0.530 | 100 |
| Native Nanite Voxelize, default 1 px | 0.704 / 848.5 / 0.607 | 0.136 / 386.1 / 0.504 | 104 |

The D3D tree timer includes camera tree selection and draw work on the GPU plus the complete brick clear, visibility and material resolve passes. The UE timer sums Nanite visibility and base pass. The passes shade through different pipelines. The corresponding full GPU frame medians for the batched mixed cut were 1.967 and 1.561 ms, for the 0.90 px GPU triangle control 1.800 and 1.153 ms, and for native Voxelize 2.174 and 2.028 ms. D3D includes its normal scene lighting and shadow work. UE runs an isolated offscreen editor capture with shadows disabled. Full-frame totals describe their respective workloads. [All moving samples](../build/oak-grove-bricks/timings.csv) · [Run medians](../build/oak-grove-bricks/summary.json)

| Camera distance | Same hierarchy, all triangles | Same hierarchy with bricks, tris / bricks | Current GPU triangle renderer, 1 px | UE Voxelize, tris / bricks |
|---|---:|---:|---:|---:|
| 50 m | 12,081,531 | 12,081,531 / 0 | 8,987,548 | 6,590,511 / 281,843 |
| 100 m | 5,768,225 | 5,503,141 / 47,068 | 4,590,224 | 290,910 / 712,001 |
| 175 m | 5,593,888 | 5,392,092 / 47,068 | 959,631 | 59,418 / 227,769 |

## What changed

The prototype replays the extracted 100 m native cut for bough 7. Each tree contributes 371 sprays, 11,767 bricks containing 30,737 occupied cells, and 378 residual triangles when its bough switches. The same asset data is shared by the four transformed instances. A projected 0.026 m error selects the replacement independently per tree. On the outward path the four switches occurred at 68, 76, 78 and 94 m. The triangle branch and shadow cut remain available. The experiment measures raster cost and selection on one frozen brick level. Epic’s full-grove counts include its own adaptive hierarchy across all canopy groups. [Decoded bake](OAK_BOUGH_VOXEL_RESULTS.md) · [Grove fixture and benchmark](../Tools/OakGroveBricks.py)

The single-sample compute shader projects each brick’s occupied sub-box to a screen rectangle, traces a ray through its 64-bit occupancy mask, and atomically selects the nearest occupied-cell midpoint and cell ID. A material pass resolves visible hits against scene depth using the bough’s stored normals and the scene’s leaf lighting and probes. One batch per material handles all selected trees, with compact, separate scratch rectangles, so overlapping trees still compete in hardware depth. The 4× MSAA path uses the box rasterizer. The hardware-box control submits another 12 proxy triangles per brick. The compute path draws two rectangle triangles per selected tree and material for resolve. Both kinds of raster proxy are excluded from the surface-triangle counts. [Brick shader](../shaders/BrickRaster.hlsl) · [Shared cell tracing and shading](../shaders/Voxel.hlsl) · [Native cluster brick tracer, UE 5.8](<C:/Program Files/Epic Games/UE_5.8/Engine/Shaders/Private/Nanite/NaniteRasterizer.usf#L1480>)

Batching reduced the isolated 100–175 m brick pass from about 0.15 ms to 0.056 ms. The box pass was 0.059 ms over that range. At 50–100 m the compute and box passes took 0.075 and 0.035 ms respectively. Our implementation still reads and clears rectangles, traces pixels already covered by other geometry, and runs a separate resolve. Native Nanite can reject a brick pixel using its depth bound before tracing. The relative cost of those remaining operations requires separate GPU timestamps. [Batched samples](../build/oak-grove-bricks/timings.csv) · [Original per-tree dispatch measurements](../build/oak-grove-bricks/unbatched-timings.csv)

## Motion and depth

The RTX 5090 rendered 3840 × 2160 at 45° vertical FOV, one sample, yaw 132°, pitch −2.235° and frozen wind. Each clip covers 26 positions outward and the same 26 inward. Timing used three rotating D3D runs and two UE runs with reversed arm order. The principal comparison has native HZB off. The silhouette target at each pose is UE Voxelize measured against the same finest UE triangle mask. A pass requires at least its foreground intersection-over-union and at most its fraction of outline pixels farther than one axial pixel from the reference outline. The mixed cut also passed the frame-to-frame mask-change comparison on all 100 within-leg transitions. Both nominal GPU and native triangle cuts exceeded the target outline error at 172 and 175 m. The separately tested 0.90 px GPU triangle setting passed all frames. The admitted cuts have visibly different numerical headroom under that gate. [Per-frame quality, counts and temporal errors](../build/oak-grove-bricks/quality.csv)

Compute and hardware boxes agreed on at least 99.9969% occupancy IoU on every moving frame. At 100 m their 95th-percentile depth difference was 0.06 mm among pixels whose depth changed relative to the reusable triangle cut. At 175 m it was 0.24 mm. A small set of pixels had metre-scale layer differences despite matching occupancy. Those pixels matter when using depth to hide another object. [Paired depth measurements](../build/oak-grove-bricks/depth.csv)

## Occlusion and the next experiment

The native HZB on/off test repeated both directions with temporal view state. At 50–100 m HZB removed 9.73% of triangle-only submissions. For Voxelize it removed 1.75% of triangles and 1.70% of bricks. At 100–175 m it removed 5.92% of triangle-only submissions, and 1.83% of triangles and 2.32% of bricks for Voxelize. Percentages are summed over the moving frames within each range. Across both assets and ranges there were zero silhouette pixel differences with HZB enabled. Visibility plus base time showed no repeatable saving in this four-tree arrangement. The HZB-on editor run reported an access violation after logging completion and closing its log. All requested images and profile lengths were verified. [Native HZB counters and masks](../build/oak-grove-bricks/occlusion.csv) · [Native HZB timing repeats](../build/oak-grove-bricks/occlusion-gpu.csv)

An offline D3D depth test placed hypothetical 8 × 8 pixel objects at three depths in each of three cameras. A tile culls only if all 64 pixels have closer foreground depth. Against the imported source-triangle depth, at 100 m with objects at 105 m, the reusable triangle cut rejected 2,119 tiles, including 130 that the source left visible. The mixed cut rejected 2,096, including 128 that the source left visible. Approximated silhouettes and first-hit depth can create false occluders in both representations. These are tile opportunities on saved images. The live four-oak D3D timings do not include this offline culler. [Tile counts at all nine camera and object-depth combinations](../build/oak-grove-bricks/depth-occlusion.csv)

Keep the qualified GPU triangle path as the performance baseline. For voxels, put the brick choice into that same GPU hierarchy, build progressively coarser levels for adjoining boughs, and measure the crossover against its triangle sibling. At 175 m this prototype still submits the same four boughs and brick count as at 100 m, and more than five million triangles remain elsewhere in the grove.

For a forest, test a shared depth pyramid with trunks, terrain, and the actual occupied canopy hits from either representation. Preserve holes when building the pyramid, use conservative depth for coarse cells, and re-test newly revealed regions after camera movement. Choose each representation by the cost of producing usable depth and the downstream work it saves. This grove demonstrates that triangle occluders also save traversal and raster work. A dense forest is needed to establish when an occlusion pass pays for itself. [UE instance occlusion](<C:/Program Files/Epic Games/UE_5.8/Engine/Shaders/Private/Nanite/NaniteInstanceCulling.usf#L394>) · [UE cluster occlusion](<C:/Program Files/Epic Games/UE_5.8/Engine/Shaders/Private/Nanite/NaniteClusterCulling.usf#L858>)

## Reproduce in this workspace

```powershell
.\Tools\BuildOakGroveBricks.ps1 -View -Arm compute -Distance 100
```

Use `-Arm gpu-qualified`, `gpu-tri`, `tri`, or `box` to open a control, and `-Distance 175` for the far pose. Use `-Benchmark` to repeat the native and D3D motion, depth and occlusion measurements. This wrapper uses the existing reusable oak hierarchy and decoded native bough fixtures under `build`. [Build and view script](../Tools/BuildOakGroveBricks.ps1)
