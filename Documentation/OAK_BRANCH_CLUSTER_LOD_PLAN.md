# Oak Branch clustered triangle LOD experiment

Use the modeled Oak Branch to find out how much geometry we can remove before the branch loses its leaf shape, openings, depth, or stability in motion. Start with fixed groups, using shoots as the analogy for the oak, then compare regrouped triangle clusters on the same asset and camera route. Keep the baker and selector applicable to general triangle meshes. This expands checkpoints 14–16 of the [engine plan](ENGINE_PLAN.md). See the [assumptions and simplifications](OAK_BRANCH_LOD_ASSUMPTIONS.md) for the starting choices and when to revisit them.

## Implementation phases and review points

1. **Bake and display forced levels.** Validate the current export and component IDs, then bake fixed groups with full detail and two coarser levels. Draw these through the existing indexed path with stable triangle and group tints. At the first review, compare forced levels on the branch and show achieved triangle reductions, open-outline preservation, and where locks caused the bake to stall.
2. **Select LOD automatically.** Add the CPU selector, projected-error readouts, and freeze cut. At the second review, compare automatic choices to the acceptable forced levels on the same approach, retreat, and orbit. Record visible transitions, shadow changes, and CPU and GPU cost.
3. **Evaluate regrouping.** Use the fixed-group results to decide whether regrouping addresses a measured seam or packing cost. Prove the shared-boundary and cut rules on the connected strip fixture before comparing the DAG to fixed groups on the branch.

## Phase I results

The baker reused the engine GLB loader and packed whole source components into 129 spatial groups. Within a component, the first recipe uses meshoptimizer 1.3 with existing vertices and locked open borders. It targets 80% and 50% of the original triangle count. The scan builder packs each group's representation into clusters with at most 128 triangles and 256 vertices. The baked levels share the original vertex buffer and draw as one foliage batch per camera or shadow pass.

| Level | Foliage triangles | Reduction | Clusters | Maximum sampled deviation in mesh-local units |
|---|---:|---:|---:|---:|
| Source and clustered LOD 0 | 123,200 | 0% | 1,032 at LOD 0 | 0 |
| LOD 1 | 98,560 | 20.0% | 774 | 0.0009110 |
| LOD 2 | 73,142 | 40.6% | 645 | 0.0140664 |

An independent check compared the baked index buffers to the GLB. Clustered LOD 0 contains every original triangle exactly once. All three levels retain the same 3,080 components in their assigned groups and the same 73,920 directed border edges. LOD 1 reached 32 triangles per leaf. LOD 2 reached 22 triangles on 389 leaves and 24 triangles on the remaining 2,691. All leaves stopped above the 20-triangle target within the current border and simplifier constraints. All clusters met both caps.

The default Release bake took about 2.5 seconds and wrote a 3.42 MiB sidecar. The Debug bake took about 64 seconds and produced the same binary hash. Incremental builds reuse the bake. Doubling the sample subdivisions from four to eight left the worst two-way deviation unchanged at the reported precision. LOD 2's reverse direction rose from 0.0117213 to 0.0136963 mesh-local units, which illustrates the sensitivity of sampled errors.

The Release viewer rendered all four representations and all three unlit tint modes with 4× MSAA. For review, compare the two full-detail modes first and then inspect the forced reductions at the same camera and paused wind pose. Look for flatter leaf interiors at close range, changes to light leaking through the branch and its ground shadow, and extra shimmer while orbiting a forced level. Use the tints for geometry inspection and Shaded for visual or timing comparisons. This single branch and the live HUD provide a visual check. A repeatable timing comparison remains open.

## Rim-collapse follow-up

The shaded review found Source GLB and LOD 0 visually identical. LOD 2 remained acceptable at the zoom limit. The visible change was a slight lighting difference from the reduced bend, with no observed additional shimmer or noticeable shadow change. Each leaf has 24 rim vertices. A triangulated disk with that rim needs at least 22 triangles, so LOD 2's 22–24 triangles were near the floor for this lock policy.

LOD 3 allows border collapses and pins the existing UV end midpoint at each end of the leaf. The tool selects those anchors through the configurable `--rim-anchors uv-v` policy for this asset. A candidate can simplify to 35% of the original triangles and have up to 10% of the anchor span in sampled two-way rim deviation. The baker keeps LOD 2 for components that fail the rim check. It records surface and rim deviation separately.

LOD 3 baked to **42,731 foliage triangles across 387 clusters**, 41.6% fewer triangles than LOD 2 and 65.3% fewer than the source. All 3,080 leaves accepted rim changes. An independent check found 13 triangles on 389 leaves and 14 on 2,691 leaves, verified both anchors on every retained rim and both cluster caps, and compared the LOD 0–2 index buffers and cluster ranges byte for byte to Phase I.

The maximum sampled surface deviation for LOD 3 was 0.0137458 mesh-local units and rim deviation was 0.0090372. Sampling at eight subdivisions per edge gave the same geometry and increased the reported surface maximum to 0.0139310. Both surface maxima are slightly below LOD 2's 0.0140664, even though only LOD 3 changes the rim. Compare silhouette, gaps, and shadows at matched poses before choosing error thresholds from these measurements.

**Copy view data** captures the current view, sun and wind, MSAA, active representation, and a group's errors. The projected readout uses a camera-space box and the sampled surface error at rest. It is a diagnostic estimate for a forced cut. A group whose bound reaches behind the near plane displays that condition instead of a finite pixel value.

The first review of LOD 3 with the flat leaf material found it acceptable at the zoom limit, with only a minor shading pop during the switch. The next comparison uses a baked leaf base-color texture from the authored Blender graph. The export keeps the branch geometry, normals, and UVs byte for byte, so the textured comparison can show whether vein placement or UV interpolation makes the same LOD change easier to see.

## UV-aware bake and next comparison

The textured review identified a visible center vein distortion in LOD 2 and found LOD 3 acceptable. The revised baker adds the material's UVs to the collapse cost. When an originally nondegenerate UV chart would acquire a zero-area UV triangle, the baker retries with a higher triangle budget and can keep the previous valid representation. Oak Branch uses weight 0.1 per UV coordinate. Stronger weights did not consistently protect the vein at the same triangle targets.

| Forced level | Previous triangles | UV-aware triangles | Previous median largest vein shift per leaf | UV-aware median largest vein shift per leaf |
|---|---:|---:|---:|---:|
| LOD 2 | 73,142 | 80,080 | 0.022114 | 0.002505 |
| LOD 3 | 42,731 | 43,120 | 0.005861 | 0.005636 |

The vein check sampled the position of the same UV centerline at 99 points on each leaf, in mesh-local units. The 95th percentile per-leaf shift for LOD 3 fell from 0.024321 to 0.007450. All 3,080 leaves survived in every level, and the baked UV triangles had positive area in their source orientation. LOD 2 reached 26 triangles per leaf with the UV check. LOD 3 reached 14 with the rim limits. Visual review of the revised textured levels is the next comparison.

An offline probe asked for 10 triangles per leaf and hit LOD 3's rim limit. The **LOD 4 - Forced far** recipe uses the same UV weight and increases the permitted sampled rim deviation from 10% to 15% of the anchor span. With the UV-area check it reaches 11 triangles per leaf and 33,880 total, 21.4% fewer than LOD 3. Its worst sampled rim deviation is 0.0397582 mesh-local units, compared with 0.0090372 for LOD 3.

At the recorded 29.075836-unit view, with a 45° vertical FOV and a 3200 × 1800 viewport, the LOD 3 to 4 swap was undetectable. The switch was slightly visible closer in.

## Area-fitted leaf representations

The opt-in `--leaf-proxies uv-v` recipe builds two new representations for the textured open leaf strips. It keeps the source tip and attachment and sums the oriented triangle area vectors, `0.5 × cross(B − A, C − A)`. The component perpendicular to the tip-to-attachment axis gives a reference normal and projected area `A`. With a fixed anchor span `L`, a four-point outline with perpendicular cross-section width `2A / L` has projected area `A`. The source side widths determine how to divide the width between left and right. UV0 assigns the rim points to the corresponding sides of the original texture and keeps the centerline at `U = 0.5`.

| Forced level | Triangles per leaf | Foliage triangles | Clusters |
|---|---:|---:|---:|
| LOD 4 | 11 | 33,880 | 371 |
| LOD 5 | 4 | 12,320 | 129 |
| LOD 6 | 2 | 6,160 | 129 |

LOD 5 uses a fan with a center vertex offset out of the reference plane to retain part of the bend. LOD 6 triangulates the same outline along the tip-to-attachment edge. Both levels retain all 3,080 leaves. The fixed group size is still capped at 24 components and the cluster cap stays at 128 triangles. The largest measured per-leaf area difference was under 0.001% along the direction used to fit it. Screen coverage through the canopy can still change with view angle, overlap, and the new outline. The sampled surface and rim distances remain available in the bake report and inspector.

### Forced comparison at 40.24 scene units

The LOD 4 to 5 change was small at this view, and LOD 5 and 6 looked virtually alike. The ground shadow changed more visibly and became slightly thinner at coarser levels. Keep LOD 5 available as a forced comparison. Try LOD 4 directly against LOD 6 on the same pose and a short orbit before choosing the initial automatic ladder.

| Level | Group 0 triangles | Sampled surface error (mesh units) | Projected estimate (px) | Sampled rim deviation (mesh units) |
|---|---:|---:|---:|---:|
| 4 | 253 | 0.02159 | 1.19 | 0.03841 |
| 5 | 92 | 0.01900 | 1.05 | 0.03249 |
| 6 | 46 | 0.02182 | 1.21 | 0.03249 |

Recorded in Shaded mode at distance 40.243382, yaw −125.5924°, and pitch 67.8383°. Eye was (−9.960587, 39.916283, −5.925095) and focus was (2.384012, 2.645970, 2.910301). Vertical FOV was 45°, viewport 3200 × 1800, and MSAA 1×. The sun was at azimuth −100.00° and elevation 54.57°. Wind strength was 1.00, paused at phase 0.0000.

The measurements are raw diagnostics for one group. LOD 5's sampled surface error is lower than LOD 4's, so include this case when verifying the selector's monotonic error envelope and shared projection bounds. Calibrate the threshold on the full branch across views. The image and shadow comparisons determine which levels belong in the automatic ladder.

### First automatic cut

The close review placed the undetectable LOD 2 to 3 swap at 17.856205 scene units, yaw −121.0087° and pitch 68.7549°, with the same focus, viewport, sun, and paused wind as the 40.24-unit review. The change could be seen closer to the branch during a toggle, and LOD 3 looked acceptable as a persistent near representation. The review also found a greater visible difference between LOD 3 and 5 than between LOD 3 and 4 near the far comparison. A source-relative sampled positional error alone gives little information about how often outlines, texture mapping, or lighting change over the leaf surfaces.

The first automatic sequence uses LOD 3 as the near fallback, LOD 4 at or below its projected error limit, and LOD 6 when both limits pass. Start at 1.70 px for LOD 4 and 1.20 px for LOD 6, guided by the recorded 29.08- and 40.24-unit views. These are trial settings. The selection error for each candidate takes the running maximum of sampled source-relative errors along the active sequence. Both candidates are projected from the same box spanning source and fitted geometry, padded by the largest participating error. Bounds at or behind the near plane keep the group at LOD 3. The raw inspector errors remain available for comparison.

Freeze cut preserves each group's current level while camera and error estimates keep updating. Use the level tints to see individual switches. Unfreeze for the retreat and approach comparison. Keep the shadow and camera cut paired during this trial. Record the nearest acceptable LOD 4 to 6 swap directly and note whether visible switching, gaps, or shadow thinning call for different thresholds.

The Release viewer check at 3200 × 1800 held 47 groups at LOD 3 and 82 at LOD 4 while moving from 28.52 to 11.39 scene units. Unfreezing selected all 129 groups at LOD 3. At 41.78 units the live cut used 8 groups at LOD 4 and 121 at LOD 6, and the 300-unit preset selected LOD 6 for all groups. An independent check against the baked index ranges reproduced both mixed cuts. Visual calibration of the switches and their shadows is still open.

The subsequent visual review reported no detectable quality loss with automatic switching on the tested views. Use that result as the visual baseline for the performance comparison.

### Run the Oak Branch benchmark

From the engine repository in PowerShell, close other viewer windows and run the Release build. Keep the benchmark window visible until it closes. Use a fresh output filename for each run to keep earlier results.

```powershell
Start-Process -FilePath .\build\phase1\Release\CustomGameEngine.exe -ArgumentList '--branch-benchmark', '.\build\oak-branch-benchmark.csv' -Wait
```

Send `build\oak-branch-benchmark.csv` back with your CPU model and any conditions that could affect the run, such as battery mode or another active GPU workload. The CSV includes the GPU adapter and all the camera, sun, wind, MSAA, and LOD settings. There is no manual timing or averaging step. A second run with a different output filename helps confirm small timing differences.

The route uses one branch and a 5 × 5 grid of the same branch mesh, spaced 11 scene units apart. Every copy shares the source, bake, and textures. The automatic cut is evaluated independently for each copy. Source GLB and the forced levels use indexed instancing. For Automatic, adjacent instances whose entire cut is the same LOD share a draw, and mixed instances batch adjacent groups using the same level. Both camera and shadow passes use the chosen cut. Every branch is submitted, including branches outside the camera view.

Each size runs Source GLB, forced LOD 3, and Automatic through the same staged route. The far stage also runs forced LOD 6. Distance ranges are 12–20, 26–54, and 100–300 scene units. Each case uses 12 warm-up frames and four passes over eight sampled poses. The route fixes the sun, wind phase, FOV, and 1× MSAA. It uses the viewer's 3200 × 1800 render target and renders without inspector or HUD overlays. Report schema 3 runs `auto_direct` and `auto_indirect` with the full group scan, plus `auto_instance` with indirect drawing and the instance-wide LOD 6 check. Their order rotates across poses and repetitions. The benchmark compares the ordered group selections before writing each case.

| CSV fields | Meaning |
|---|---|
| `groups_lod3`, `groups_lod4`, `groups_lod6` | Number of instance/group selections by level. Source GLB has no fixed-group selections. |
| `*_draws`, `*_triangles` | Draw calls and submitted triangles for that pass, including instanced copies. The `*_foliage_*` columns isolate the leaf mesh from bark and ground. |
| `*_foliage_calls` | Foliage draw API calls recorded by the CPU. One `ExecuteIndirect` call can execute multiple indexed draws. |
| `cut_hash` | Hash of the ordered instance/group selections in Automatic mode. Pair by instance count, segment, repetition, and pose. |
| `instance_lod_checks`, `instance_lod6_accepts`, `group_lod_checks` | Instance bounds tested, whole copies accepted at LOD 6, and groups that needed their own error projection. |
| `cpu_select_ms`, `cpu_submit_ms` | Cut computation and CPU work to prepare, record, and submit the frame. `cpu_draw_list_ms` is the draw-list portion of submission time. |
| `gpu_camera_ms`, `gpu_shadow_ms`, `gpu_frame_ms` | GPU time for the camera pass, shadow pass, and full frame. The camera scope includes clears and any resolve. |
| `wall_frame_ms` | Elapsed frame time including present and the GPU fence wait. Display refresh can dominate this value. |

Compare modes at the same instance count and route segment using median and 95th-percentile frame costs. The larger scene also changes the fitted shadow volume. Compare per-pass timings within each scene size to separate draw-call cost from foliage reduction.

### Indirect drawing results

Automatic builds its indexed ranges once per frame and puts them in LOD order. Both backends use the same lists, and both passes reuse them. The indirect path binds state and calls `ExecuteIndirect` once per populated LOD. Each command carries its indexed arguments and triangle tag offset. A cut consisting of one indexed draw uses the direct call. **Indirect draws** is enabled by default and can be toggled on a frozen cut.

Two Release runs on the Ryzen 7 9800X3D and GeForce RTX 5090 used the same benchmark route at 3200 × 1800 and 1× MSAA with the window hidden. All 192 paired cuts per run matched their ordered cut hashes, group totals, indexed draw counts, and per-pass triangle counts. Draw and triangle counts also matched the original benchmark. Triangle tints and the visible shadow matched by eye in a frozen, mixed cut in the Debug viewer.

| 25-copy test | Direct API calls per foliage pass | Indirect API calls per foliage pass | CPU submit median, direct → indirect | CPU submit p95, direct → indirect | GPU frame median, direct → indirect |
|---|---:|---:|---:|---:|---:|
| Mixed segment, 64 pairs | 168–518 | 2–3 | 0.168 → 0.080 ms | 0.313 → 0.164 ms | 0.187 → 0.184 ms |
| 38-unit pose, 8 pairs | 518 | 3 | 0.180 → 0.091 ms | 0.720 → 0.147 ms | 0.195 → 0.196 ms |
| Far segment, 64 pairs | 1 | 1 | 0.073 → 0.073 ms | 0.144 → 0.143 ms | 0.102 → 0.104 ms |

At 38 units both backends executed 518 indexed draws and 778,687 foliage triangles per pass. The mixed-segment CPU submission improvement reproduced in each run. Median submission fell from 0.153 to 0.078 ms in the first and from 0.183 to 0.087 ms in the second. Selection stayed near 0.118 ms median for 25 copies and GPU differences were small. The 38-unit p95 is sensitive to outliers because it has eight samples per backend. Raw reports are `build/phase1/oak-branch-indirect-a.csv` and `build/phase1/oak-branch-indirect-b.csv`.

### Instance-wide LOD 6 check

At scene load the renderer caches the padded group projection bounds and both transition errors. It encloses those group boxes in a single mesh-local box and takes the maximum error for each transition. Automatic projects this box once per copy using its placement and the camera view. If both projected limits pass, assign LOD 6 to the whole copy. A bound that intersects the near plane or exceeds a limit goes through the regular group checks. The test includes a small floating-point margin and keeps using the existing sampled surface errors.

Two Release runs on the Ryzen 7 9800X3D and GeForce RTX 5090 compared the shortcut with the full group scan at 3200 × 1800 and 1× MSAA. Both variants used the cached group data and indirect drawing. Each run checked 192 sets of Automatic frames and matched every selected group. The resulting cut hashes, indexed draw counts, and foliage triangles also matched the previous benchmark.

| 25-copy segment | Group projections per frame with shortcut | Median selection without shortcut | Median selection with shortcut |
|---|---:|---:|---:|
| Near | 3,225 | 0.1044 ms | 0.1049 ms |
| Mixed | 1,161–3,225 | 0.1085 ms | 0.1070 ms |
| Far | 0 | 0.1055 ms | 0.0019 ms |

At every far pose, 25 instance checks replaced 3,225 group projections. Far p95 selection time fell from 0.2212 to 0.0037 ms. At 54 scene units the shortcut passed for 16 copies and skipped 2,064 group projections. It passed for zero copies in the first five mixed poses, so the mixed-segment median shows little change. Raw reports are `build/phase1/oak-branch-instance-a.csv` and `build/phase1/oak-branch-instance-b.csv`.

### Shadow LOD calibration

**Shadow LOD** can follow the camera cut or force foliage to LOD 3, 4, or 6. Each forced level can apply to all casters or to branches whose wind-expanded bounds lie wholly outside the camera frustum. A branch can still cast a visible shadow in either mode. Bark always uses its full mesh. **Shadow factor view** shows received light in grayscale and works with a frozen camera cut.

```powershell
Start-Process -FilePath .\build\phase1\Release\CustomGameEngine.exe -ArgumentList '--branch-shadow-benchmark', '.\build\oak-branch-shadow.csv' -Wait
```

The route uses the 25-copy grid at 20, 35, and 120 scene units, sun elevations of 27° and 68°, and still foliage followed by eight matched wind phases. Each comparison freezes the camera cut and holds shadow visibility, bias, and the light projection fixed. Five shaded timings per case follow a warm-up. Separate 1× renders measure shadow visibility in the 3200 × 1800 camera image. The report splits ground and branch receivers and records changed pixels, changes of about 25 percentage points or more, and light gained and lost in equivalents of fully lit pixels. The temporal metric sums changes in the candidate-minus-reference shadow difference between successive wind samples while the receiver class stays the same. The shadow union counts pixels shadowed by either version and provides a denominator for coverage differences.

Two Release runs on the Ryzen 7 9800X3D and GeForce RTX 5090 matched on every image metric and count across all 378 rows. Median shadow times for the 35-unit pose were 0.067–0.070 ms with the camera cut and 0.059–0.061 ms with LOD 6 on off-camera casters only. At that pose, six of the 25 casters used the override, reducing shadow foliage from 909,001 to 687,310 triangles. Foliage API calls rose from three to seven because the off-camera copies occupied four separate instance ranges. Forcing LOD 6 everywhere submitted 154,000 foliage triangles in one instanced draw and took 0.039–0.040 ms, with larger differences in the shadow factor.

Across the sampled views, off-camera LOD 6 changed zero branch-receiver pixels. Its worst ground sample changed 1,029 pixels by roughly 25 percentage points or more, about 0.08% of the ground shadow union. In the still 35-unit low-sun case, forcing LOD 6 everywhere changed 102,811 ground pixels and 65,505 branch pixels by that amount. The distant camera already selected LOD 6 for every group. The comparison uses today's mixed cut as the reference. Perceptibility in shaded mode requires a visual check.

The diagnostic projects each sampled bake error through the largest XY magnification of the orthographic light transform in texels. It estimates the footprint of the displacement at rest. For LOD 6 it reached 0.37–0.70 texels in this grid, and that estimate was the same for both scopes even though their image differences were very different. Keep **Match camera cut** as the default and use off-camera LOD 6 as the first candidate to retest on full crowns or larger scenes, especially if the light projection or shadow resolution changes. Compare the same coverage and temporal metrics and check the transition when a caster crosses the camera edge before enabling a runtime policy. Raw reports are `build/phase1/oak-branch-shadow-a.csv` and `build/phase1/oak-branch-shadow-b.csv`.

### Voxel bake phase I

`BakeVoxelFoliage` is an offline C++ tool. It loads a named GLB mesh, finds triangle-cell overlaps, and measures coverage in three axial and four diagonal directions. Each ray is clipped to the cell and counts once even if it hits several triangles. The tool writes an any-overlap control and binary occupancy at the requested cutoffs for the mean of the seven hit fractions. An optional CSV records directional hit counts per candidate cell. See the [bake assumptions](OAK_BRANCH_LOD_ASSUMPTIONS.md).

```powershell
cmake --build build/phase1 --config Release --target BakeVoxelFoliage VoxelBakeTests
ctest --test-dir build/phase1 -C Release --output-on-failure
.\build\phase1\Release\BakeVoxelFoliage.exe .\assets\LowBranchedWhiteOak-SingleBranch.glb 'Oak branch - modeled leaves' .\build\phase1\oak-branch-voxels --cell-size 0.03125 --samples 8 --cell-report
```

The default run bakes three sizes, doubling the cell edge each time, and sweeps cutoffs of 0.05, 0.10, and 0.20. Repeat `--cutoff` to supply custom values. `--samples` is the number of samples per side of each direction's projected rectangle. Diagonal probes exclude samples outside the projected cell. `voxel-bake.txt` maps the filenames to the cell sizes and cutoffs.

| Cell edge (mesh units) | Any-overlap cells | 0.05 cutoff | 0.10 cutoff | 0.20 cutoff |
| ---: | ---: | ---: | ---: | ---: |
| 0.03125 | 138,694 | 97,511 | 81,286 | 57,391 |
| 0.0625 | 39,731 | 26,254 | 20,753 | 13,410 |
| 0.125 | 10,815 | 6,959 | 5,263 | 2,989 |

Both Debug and Release passed the fixtures for thin and grid-aligned surfaces, clipped ray depths, layered patches, signed brick coordinates, asset round-tripping, and truncated data rejection. Two Oak Branch runs with 8 × 8 probes produced byte-identical `.voxels` assets and per-cell CSVs across all three sizes. An independent read confirmed the source binding, shared origin, and nested cutoff masks. Moving to 16 × 16 probes changed 1.7–4.3% of occupied cells across the thresholded variants, measured over the union of their occupied cells. The any-overlap masks were identical. Choose cutoffs in Phase II by comparing the locations of gaps and depth differences, with the denser bake as a check on sampling sensitivity.

The files use little-endian `VOXELFOL` version 1. The header stores the source fingerprint, mesh index, origin, cell size, and brick count. Each brick stores three signed coordinates and a 64-bit mask with local bit index `x + 4y + 16z`. The assets contain binary occupancy. Material sampling and the camera rendering path are later phases.

### CPU voxel comparison phase II

`CompareVoxelFoliage` uses the viewer's Oak Branch focus, the 45° vertical FOV, and a fixed route of 18 poses. It traces the original foliage triangles, LOD 6, and each supplied bake at the same sample positions. The triangle path tests the nearest two-sided surface. The voxel path intersects sparse brick bounds and steps through cells to return the first occupied cell entry. Bark clips the same rays in every comparison. These geometry-only measurements trace foliage at rest. They omit lighting, shadows, and ground.

```powershell
cmake --build build/phase1 --config Release --target CompareVoxelFoliage FoliageTraceTests
ctest --test-dir build/phase1 -C Release --output-on-failure
.\build\phase1\Release\CompareVoxelFoliage.exe .\build\phase1\Release\assets .\build\phase1\oak-branch-voxel-compare .\build\phase1\oak-branch-voxels .\build\phase1\oak-branch-voxels-denser --samples 2
```

The viewport is 800 × 450 and `--samples` sets the number of sample points per pixel side. The main route uses a 2 × 2 grid and includes a retreat from 18 to 240 scene units, an orbit at 60 units, and two held-back angles. `--samples 4 --image-poses-only` reruns the five poses with PNG output using a 4 × 4 grid. PNGs share a crop derived from the source silhouette. Green shows foliage coverage, red shows gained coverage, and blue shows lost coverage. Signed depth images use red for early hits and blue for late hits, with saturation at 0.2 scene units.

The CSV measures over the full viewport. Coverage error is the fraction of sample rays where one version hits and the other misses, among rays hit by either version. Depth error is a difference in nearest hit distance along rays hit by both versions. An enclosed gap is a four-connected group of at least four pixels with at most 25% foliage coverage that never reaches the viewport edge. It counts as closed when fewer than half its pixels are still mostly clear. Gain and loss maps also show changes to openings that connect to the exterior.

LOD 6 matched source coverage more closely than every Phase I voxel variant on all 18 poses. Raising the cutoff on the 0.03125 grid traded excessive density for missing coverage. At a 0.25 cutoff that grid had nearly zero net coverage change on the retreat, while visible coverage still differed on 12.7% of the hit-ray union on average. LOD 6 averaged 8.8% on the same route.

I added one smaller cell size at 0.015625 mesh units and swept 0.20–0.35. The 0.25 cutoff kept the retreat's net coverage close to the reference and closed fewer orbit gaps than the 0.20 cutoff. With the 16 × 16 bake probes and 4 × 4 camera samples, it kept all counted source gaps at the five diagnostic poses. Coverage error for those poses was as follows.

| Pose | LOD 6 | 0.015625 cells, 0.25 cutoff | 0.03125 cells, 0.25 cutoff |
| --- | ---: | ---: | ---: |
| Retreat, 40 units | 8.48% | 7.61% | 12.16% |
| Retreat, 120 units | 8.83% | 7.80% | 12.15% |
| Orbit, 60 units | 11.08% | 12.76% | 19.35% |
| Side holdout | 10.58% | 14.41% | 21.62% |
| Back holdout | 8.41% | 7.35% | 11.66% |

Carry the **0.015625 / 0.25** bake into Phase III as the higher-quality candidate. It has 29,872 bricks and a 597,500-byte file. Also carry **0.03125 / 0.25** as a lower-workload candidate with 7,891 bricks and a 157,880-byte file. The coarser bake's side and orbit errors make it useful for checking what breaks as brick count falls. These are offline file sizes. The GPU timing and resident memory comparisons are still open. The two files are `build/phase1/oak-branch-voxels-finer-denser/level-0-cutoff-2.voxels` and `build/phase1/oak-branch-voxels-higher-cutoffs/level-0-cutoff-1.voxels`.

The full route reports are under `build/phase1/oak-branch-voxel-compare`, `oak-branch-voxel-compare-higher-cutoffs`, and `oak-branch-voxel-compare-finer`. The 4 × 4 validation reports are in `oak-branch-voxel-compare-dense-rays`, `oak-branch-voxel-compare-higher-dense-rays`, and `oak-branch-voxel-compare-finer-dense-rays` under the same build directory. For finer-cell rebakes use `--cell-size 0.015625 --levels 1 --samples 16 --cutoff 0.20 --cutoff 0.25 --cutoff 0.30 --cutoff 0.35`. For the higher-cutoff sweep at the original sizes use `--cell-size 0.03125 --samples 16` with cutoffs 0.25 through 0.45 in 0.05 increments.

### GPU voxel comparison phase III

The Oak Branch and Oak Branch Grid viewers have forced source, LOD 6, fine voxels, and coarse voxels in **Branch LOD → Voxel comparison**. The build bakes the selected 16 × 16 probe recipes at a 25% cutoff. The comparison holds the foliage at rest and uses LOD 6 triangles for foliage casting in all four modes. Bark and ground use the normal depth path. Voxels use average leaf color and simple lighting in shaded view. Use **Coverage view** to compare shape under a shared flat foliage color.

The D3D12 path submits one indexed box per nonempty brick. It rasterizes the far faces as proxies, traverses the brick in the pixel shader, and writes depth at the first occupied cell. Rasters with no hit leave the underlying camera image available. The 4× mode evaluates coverage at each sample. This prototype uploads the brick data once and uses instancing across branch copies. It uses the renderer's upload heaps for the indexed and brick data. The scene's branch frustum test still applies, while individual bricks rely on rasterizer clipping.

```powershell
Start-Process -FilePath .\build\phase1\Release\CustomGameEngine.exe -ArgumentList '--branch-voxel-benchmark', '.\build\oak-branch-voxel-gpu.csv' -Wait
```

The benchmark first checks GPU coverage and encoded view depth against the CPU tracer in three poses per voxel size with 1× sampling. The inside-brick case checks every fourth pixel per axis. The route then times LOD 6 and both voxel sizes at 18–300 scene units, with one and 25 branch copies, 1× and 4× sampling, and both coverage and shaded views. Each pose has nine timed samples per mode after warm-up, and the mode order rotates across repetitions. The CSV records camera and shadow times, submitted foliage triangles, and visible branches. The shadow caster set and foliage triangle count are checked for equality across the modes.

Two Release runs on the Ryzen 7 9800X3D and GeForce RTX 5090 matched on all 1,728 non-timing rows per run and reproduced the parity report exactly. Across the six parity checks, coverage disagreement reached at most 0.181% of the hit-ray union. The 99th-percentile absolute depth difference stayed below 0.000354 scene units. The original triangle benchmark also reproduced all 1,024 non-timing rows.

Neither box-proxy voxel bake beat LOD 6 in shaded mode at any sampled distance in either run. The occasional single-copy coverage-only wins with 4× sampling did not repeat. These are pooled medians for the 3200 × 1800 shaded camera pass at 4× sampling.

| Branches in scene | Distance | LOD 6 | Fine voxels | Coarse voxels |
| ---: | ---: | ---: | ---: | ---: |
| 1 | 40 | 0.087 ms | 0.141 ms | 0.122 ms |
| 1 | 120 | 0.036 ms | 0.063 ms | 0.049 ms |
| 1 | 300 | 0.034 ms | 0.069 ms | 0.042 ms |
| 25 | 40 | 0.177 ms | 1.113 ms | 0.687 ms |
| 25 | 120 | 0.111 ms | 1.089 ms | 0.339 ms |
| 25 | 300 | 0.090 ms | 1.069 ms | 0.324 ms |

At 120–300 units all 25 copies pass the camera frustum test. They submit 154,000 LOD 6 foliage triangles, 8,961,600 fine proxy triangles, or 2,367,300 coarse proxy triangles. The projected brick footprints shrink on the retreat, while the amount of submitted brick geometry holds steady. Keep LOD 6 as the current runtime choice. Any voxel performance follow-up would need to reduce the per-brick rendering cost and verify the coverage gains again. Raw data are `build/phase1/oak-branch-voxel-gpu-a.csv`, `oak-branch-voxel-gpu-b.csv`, and the matching `*-parity.csv` files.

### Binary voxel far-distance comparison

The Oak Branch and Oak Branch Grid viewers expose all four timed far voxel bakes at the 25% cutoff. Under **Branch LOD → Voxel comparison**, choose **LOD 6 triangles** or a voxel size, then choose a **Camera distance** and angle. The controls use the benchmark's depth range at 300 units and beyond. Compare **Coverage view** first if you want to isolate canopy shape from shading. Use **Copy view data** to capture the pose where a change becomes visible.

The optional far benchmark accepts two source-matched binary bakes and forces each across the foliage of all visible branches in turn. The camera route is 300, 600, 1,200, 1,800, 2,400, and 3,000 scene units. It uses a 20–3,600 depth range for precision in this fixture. The normal viewer uses its existing clip planes. The experiment fixes foliage shadows at LOD 6, holds the branch at rest, and keeps the original bark draw. It checks that all instances remain visible and that triangle and brick submissions match the requested bake.

```powershell
Start-Process -FilePath .\build\phase1\Release\CustomGameEngine.exe -ArgumentList '--branch-voxel-benchmark', 'far.csv', 'candidate-a.voxels', 'candidate-b.voxels' -Wait
.\build\phase1\Release\CompareVoxelFoliage.exe .\build\phase1\Release\assets quality-output candidate-directory --far --samples 8
```

The CPU route traces a centered 192 × 192 window of the same 3200 × 1800 projection and adds side and rear views and two subpixel offsets. Resolve by averaging the sample hits in each pixel. The sum of absolute resolved coverage differences divided by the source's total resolved coverage measures visible gains and losses per pixel. Signed coverage bias measures overall thickening or thinning. The binary occupancy payload and runtime sampling are the same as for Phase III. The 4 × 4 and 8 × 8 CPU grids evaluate coverage offline.

Four GPU runs on the RTX 5090 covered the four cell sizes below at a 25% bake cutoff. Each voxel comparison was timed twice with nine samples per mode, per pose, and per combination of shading, copy count, and MSAA. Each pair reproduced all 1,296 timing-free rows and its GPU/CPU parity report. The 1× parity checks had no coverage disagreements at 3,000 units and a worst 99th-percentile view-depth difference of 0.00925 units. The original GPU benchmark and original CPU route reproduced the checked 1,728 and 234 timing-free rows respectively.

These are pooled medians at 3,000 units for the shaded camera pass with 25 branches and 4× MSAA. Coverage error comes from the five single-branch 3,000-unit views with 64 offline samples per pixel.

| Representation | Bricks per branch | Submitted foliage triangles, 25 branches | GPU camera | Mean / worst resolved coverage error |
| --- | ---: | ---: | ---: | ---: |
| LOD 6 | — | 154,000 | 0.079 ms | 5.1% / 7.5% |
| 0.0625-unit cells | 1,938 | 581,400 | 0.118 ms | 12.9% / 36.9% |
| 0.125-unit cells | 398 | 119,400 | 0.068 ms | 15.5% / 37.3% |
| 0.25-unit cells | 60 | 18,000 | 0.056 ms | 22.6% / 23.9% |
| 0.5-unit cells | 12 | 3,600 | 0.055 ms | 29.1% / 36.5% |

The 0.125- and 0.25-unit bakes were already faster at 300 units on the 25-copy 4× route, the first distance tested with those bakes. At 300 units their resolved coverage errors on the retreat were 24.4% and 38.7%, against 3.9% for LOD 6. At 3,000 units the 0.125-unit bake's retreat error fell to 9.2%, but from the side coverage increased from 3.94 to 5.41 pixel equivalents. The 0.25-unit candidate was less sensitive to that view angle and reduced retreat coverage from 10.58 to 8.48 pixel equivalents. The 0.5-unit bake reduced camera time by about another microsecond while increasing average coverage error. With one branch there was no repeatable timing win. These results establish a cost advantage for batched far branches with the current rasterizer. A visual acceptance distance is still open. The quality measurements cover shape at rest and the GPU preview still uses average leaf color. A tree or stand represented by one aggregate bake needs its own comparison.

To extend the fixed groups, give each group complete triangle and voxel candidates for the same source components, using a representation tag, draw range, projection bounds, and calibrated error. Select the cheapest candidate that passes the group's screen-space error budget, then batch triangle and voxel groups through their existing paths. Include a coverage check when calibrating errors because short surface displacement can still thicken a sparse canopy. A coarse bake across several groups needs one replacement decision covering all child groups. For a future regrouped DAG, store that same replacement set and allow either geometry type in the cut.

Raw results are in `build/phase1/oak-branch-voxel-far-gpu-{a,b,c,d}.csv` and `build/phase1/oak-branch-voxel-far-quality-dense/metrics.csv`. The candidate directory records size and coverage cutoff in the filenames.

### Coverage-optimized binary bake

The optimized 0.25-unit comparison uses the same grid as the original cutoff bake. Its seed is the 25% cutoff mask. An any-overlap bake defines eligible cells. The fitting tool traces the original foliage with bark occlusion at 16 camera poses using 16 rays per pixel. It caches the rays that pass through each eligible cell and counts the active cells on each ray. Adding or removing a cell changes coverage only when that ray's count crosses zero.

The optimizer scores the mean and worst resolved pixel coverage error across fitting views, plus the worst normalized absolute change in total coverage. It keeps improvements from cell edits and tries replacing low-value bricks under a cap of 60 nonempty bricks. The selected mask uses two brick swaps and occupies 60 bricks. The comparison tool measures the result separately on the 11 earlier far views at 64 samples per pixel. The bias penalty was added after the first comparison showed that the side views became denser.

| Resolved coverage error | LOD 6 | 0.25 cutoff | 0.25 coverage optimized |
| --- | ---: | ---: | ---: |
| Mean across 11 views | 4.6% | 26.5% | 21.8% |
| Retreat at 300 units | 3.9% | 38.7% | 37.1% |
| Retreat at 1,200 units | 3.3% | 28.1% | 21.6% |
| Retreat at 3,000 units | 4.3% | 23.0% | 15.7% |
| Side at 3,000 units | 7.5% | 22.2% | 26.2% |

The optimized mask recovers missing area on the retreat. At 300 units its net coverage deficit drops from 20.4% to 6.3%, while the difference in where that coverage lands is still substantial. At 3,000 units the side view gains 14.3% above source coverage, versus 6.3% with the cutoff. The shape comparison is ready for visual review. Quality in motion and a switch distance are still open.

GPU timings used the shaded camera pass with 25 branches and 4× MSAA on the RTX 5090. At 1,200 units the pooled medians were 0.110 ms for LOD 6, 0.071 ms for the cutoff mask, and 0.071 ms for the optimized mask. At 3,000 units they were 0.080, 0.055, and 0.055 ms respectively. Both voxel masks submit 720 proxy triangles per branch. The two runs reproduced all 1,296 timing-free rows and agreed with CPU sample coverage at all three parity poses. A repeated fit reproduced the saved mask and fitting report byte for byte.

Build the viewer and the optional bake target with `cmake --build build/phase1 --config Release --target CustomGameEngine oak_branch_coverage`. The target places the fitted masks beside the other viewer assets. The fit reports are under `build/phase1/Release/bakes/coverage`. The 0.25-unit comparison images and `metrics.csv` are under `build/phase1/oak-branch-coverage-holdout`. GPU runs are `build/phase1/oak-branch-coverage-gpu-a.csv` and `oak-branch-coverage-gpu-b.csv` in the same build directory.

#### Fitting the 0.0625- and 0.125-unit masks

The finer bakes add a fitting route with closer and more oblique side views, based on the difference observed between the 150-unit retreat and rear views and the 1,800-unit side view. The fitting route uses 16 rays per pixel. The comparison uses 64 rays per pixel and 14 poses excluded from fitting, including two views near the 375-unit side preset.

| Cell size | Bricks, cutoff → fitted | Mean pixel coverage error, cutoff → fitted | Side at 375 units, cutoff → fitted |
| --- | ---: | ---: | ---: |
| 0.0625 | 1,938 → 1,812 | 20.9% → 15.4% | 31.2% → 18.0% |
| 0.125 | 398 → 365 | 27.5% → 23.2% | 36.8% → 29.2% |

At 375 units from the side, the fitted masks reduce net over-coverage from 29.5% to 7.8% and from 24.1% to 5.7%, respectively. The 0.0625-unit mask's error at 150 units rises from 15.5% to 21.1% on the retreat and from 15.2% to 20.8% from the rear. The 0.125-unit mask also has a small increase in the near retreat and rear views. Both still have measurable coverage differences from LOD 6. See `build/phase1/oak-branch-coverage-review/metrics.csv` and the corresponding pose directories.

In the visual review, the optimized 0.125-unit bake matched LOD 6 at about 500 units on the retreat and from the rear, 530 units at the side preset, and 700 units at other tested side angles. Use 700 scene units as the conservative whole-branch calibration point for an initial automatic switch on this fixture. Verify the transition through an orbit and calibrate the corresponding screen-space test before applying it to individual groups or other assets.

The **Voxels 0.0625**, **Voxels 0.125**, and **Voxels 0.25** inspector entries load the optimized mask when available and show **Coverage-optimized bake** under the cell size. The 0.25-unit override is byte-identical to the previous coverage-optimized mask. The cutoff assets are retained as inputs and benchmark controls.

In two GPU runs with 25 copies and 4× MSAA, the 0.125-unit fitted mask took 0.078 ms in the shaded camera pass at 1,200 units, versus 0.079 ms for the cutoff mask and 0.107 ms for LOD 6. The 0.0625-unit fitted mask matched the CPU's covered pixels at the GPU parity poses. One depth outlier at 3,000 units was 0.297 units, so the parity gate stopped that run before timing. See the `oak-branch-coverage-eighth-gpu-*.csv` and `oak-branch-coverage-sixteenth-gpu-a-parity.csv` reports.

### Two-normal voxel shading experiment

Open **Oak Branch Orbit**, select **Voxels 0.125**, turn off **Coverage view**, and toggle **Baked leaf normals** at a held pose. Repeat the 700-unit orbit with the default sun azimuth and with the sun on the opposite side. Both modes use the same 365-brick occupancy, fixed LOD 6 triangle shadows, and shared leaf color. Build `oak_branch_normals` to regenerate the optional sidecar.

The bake traces both ends of each of the original seven sets of cell probes and interpolates source vertex normals at the first hit from each end. It clusters the samples into two axes using absolute dot product because the leaves are two-sided. Each occupied cell stores two octahedrally encoded axes and an 8-bit relative weight in 8 bytes. The shader uses the same mixture from all viewing directions, faces each axis toward the camera, evaluates the leaf diffuse and transmission response, and weights the two responses. It samples shadows at the voxel hit and uses the shared leaf color.

The sidecar is 15,124 bytes for 1,888 cells. For 27 cells that no probe sampled, the bake used the normal from an intersecting source face. A repeated bake reproduced the sidecar byte for byte. The optimized occupancy matched the earlier orbit asset byte for byte. A held-pose GPU test at 700 units captured identical Coverage views and different shaded pixels with normals enabled under both sun azimuths. Both timing runs reproduced the same 648 timing-free rows and parity report.

| Shaded GPU camera pass, 25 copies at 4× MSAA | LOD 6 | Shared voxel lighting | Baked leaf normals |
| --- | ---: | ---: | ---: |
| 700 units | 0.1086 ms | 0.0839 ms | 0.0847 ms |
| 1,200 units | 0.1055 ms | 0.0788 ms | 0.0792 ms |
| 3,000 units | 0.0783 ms | 0.0656 ms | 0.0664 ms |

Run `--branch-voxel-normal-benchmark report.csv` to reproduce the timing. Raw runs are under `build/phase1/oak-branch-normal-gpu-{a,b}.csv`. The shaded orbit still needs visual review for lighting variation and stability.

### Voxel lighting diagnosis

The **Lighting diagnostic** control shows diffuse and backlighting or received shadow visibility. The `--branch-lighting-diagnostics report.csv` command captures LOD 6 and the optimized 0.125 bake with baked normals at seven orbit times, two sun azimuths (−100° and +80°), and distances of 250 and 700 scene units. The comparison uses one branch at rest, LOD 6 shadow casters, and 1× samples in a 3200 × 1800 viewport. Each row uses only samples both representations cover. It decodes base and shaded RGB to linear light and checks that the diffuse capture reproduces the measured unshadowed shading within RGBA8 quantization tolerance. The two runs produced byte-identical reports.

| Distance | Mean voxel base luminance vs. LOD 6 | Mean voxel luminance with shadow receiving off | Mean voxel luminance with shadow receiving on |
| --- | ---: | ---: | ---: |
| 250 units | +0.08% | −1.43% | −2.36% |
| 700 units | 0.00% | −1.42% | −3.20% |

At the 700-unit retreat with the default sun, voxel luminance was 2.31% lower without receiving shadows and 9.65% lower with them. Across all poses the unshadowed difference ranged from 7.09% darker to 4.34% brighter. The direct-light gain needed to match each pose's mean ranged from 0.91 to 1.15. Simulating gains of 2.5%, 5%, 10%, and 15% selectively on the direct-light term increased average per-pixel luminance error in each sweep.

Received voxel shadow visibility averaged 2.11 percentage points below LOD 6. Raising the voxel depth bias by half a shadow texel reduced that mean gap to 0.38 points. One texel raised mean visibility 1.08 points above LOD 6 and reduced mean absolute shadow-visibility error from 0.3282 to 0.3142, but only reduced mean absolute shaded luminance error from 0.01836 to 0.01803. The diagnostics expose the pose-dependent discrepancies and use zero additional bias in normal viewing. Raw paired results are in `build/phase1/oak-branch-lighting-diagnostics.csv`.

### View-dependent voxel normal weights

Enable **View-dependent weights** under **Baked leaf normals** in Oak Branch Orbit to compare the directional and pooled weights. The bake assigns each first-hit normal from the six signed axial probe directions to one of the two existing axes. Each direction uses eight prior samples from the pooled distribution to stabilize weights where few rays hit. The shader selects the three signed weights facing the camera and blends them in proportion to the absolute components of the local view direction. The version 2 sidecar adds 15,104 bytes for 1,888 cells including padding.

Across the 28 paired lighting captures at 250 and 700 units, mean absolute linear luminance error changed from 0.01447 to 0.01450 with shadow receiving off and from 0.01836 to 0.01831 with it on. At the 700-unit retreat with the default sun, the voxels still averaged 2.3% darker than LOD 6 with receiving off and 9.7% darker with it on. Coverage, base color, and shadow visibility matched the pooled run. The viewer defaults to pooled weights.

| Median shaded GPU camera pass on RTX 5090, 25 copies at 4× MSAA | LOD 6 | Pooled weights | Directional weights |
| --- | ---: | ---: | ---: |
| 700 units | 0.1083 ms | 0.0850 ms | 0.0856 ms |
| 1,200 units | 0.1054 ms | 0.0798 ms | 0.0799 ms |
| 3,000 units | 0.0781 ms | 0.0654 ms | 0.0661 ms |

Append `--directional` after the report path for `--branch-lighting-diagnostics` to capture directional shading or for `--branch-voxel-normal-benchmark` to time pooled and directional shading on the same mask. Raw reports are in `build/phase1/oak-branch-lighting-directional.csv` and `build/phase1/oak-branch-directional-gpu-{a,b}.csv`. Repeated lighting captures matched byte for byte, including across Debug and Release.

### Baked voxel shadow receivers

At pixels covered by both representations, reconstructing voxel color with the LOD 6 shadow visibility and the original voxel base color and diffuse term lowered average shaded luminance error from 0.01836 to 0.00791 across 28 captures. At the 700-unit retreat pose with the default sun, the mean luminance deficit fell from 9.6% to 1.6%. This substitution uses the triangle render as a diagnostic reference.

The receiver bake selects the first-hit sample nearest the weighted centroid of the existing leaf probes in each cell. The version 3 sidecar packs three 5-bit cell-local coordinates and a validity bit into the spare upper half of the last word. It stores a receiver for 1,861 of 1,888 occupied cells. The remaining 27 use cell entry. During rendering, the shader transforms the baked point with the mesh and instance transforms and uses it for the shadow lookup. A paired GPU test matched coverage, camera depth, and unshadowed color byte for byte with the option enabled and disabled.

| Mean absolute shaded luminance error | Cell entry | Baked leaf point | Reference visibility |
| --- | ---: | ---: | ---: |
| 250-unit orbit, two sun angles | 0.01850 | 0.01848 | 0.00793 |
| 700-unit orbit, two sun angles | 0.01821 | 0.01804 | 0.00789 |
| 700-unit retreat, default sun | 0.02135 | 0.02272 | 0.00910 |

The baked point improved 16 of 28 captures and increased the 700-unit retreat brightness deficit from 9.6% to 12.3%. At 700 units with 25 branches and 4× MSAA, median GPU camera time on RTX 5090 was 0.0855 ms at cell entry, 0.0865 ms with the baked point, and 0.1088 ms for LOD 6. The **Baked leaf shadow positions** inspector toggle defaults off.

Run `--branch-lighting-diagnostics report.csv` for the visibility substitution and receiver comparisons. Run `--branch-voxel-normal-benchmark report.csv --shadow-receiver` for paired GPU timings. The capture and timing reports are under `build/phase1/oak-branch-shadow-receiver-*.csv`.

### Automatic triangle-to-voxel transitions

Use **Oak Branch Transition** with 4× MSAA for the rest-pose comparison. **Automatic branch** lets the existing triangle selector choose LOD 3, 4, or 6, and switches to the coverage-fitted 0.125 bake once the branch qualifies for LOD 6 and its conservative projected cell footprint falls below 0.373 pixels. The exit footprint is 5% higher to avoid rapid switching near the threshold. The selector measures the footprint under the current projection and instance scale. At 3200 × 1800 and a 45° vertical FOV, the single branch switched at sampled distances of 733–736 on retreat and 697–699 on approach. The camera cut and triangle shadow cut are submitted separately. Foliage at the switch continues to cast LOD 6 triangle shadows.

**Automatic regions** divides the 129 spatially ordered triangle groups into four contiguous regions. The baker assigns each occupied cell of the verified whole-branch mask to the region with the strongest sampled source coverage for that cell. It stores one owner byte per cell and references the exact mask and fixed LOD file by fingerprint. Each region tests its own bounds and replaces all its triangle groups together. A fully voxelized copy uses the original whole-branch bake. Different regions can touch the same brick, which increases the number of submitted brick boxes during a partial switch. Whole-branch and regional voxel batches pack visible placements into one draw per active mask and keep the usual indirect triangle lists.

Use **Find mixed regions** with a single branch to land at a pose where both representations are selected. **Tint triangles and voxels** makes the boundary visible. **Freeze cut** holds the selected triangle levels and voxel regions as the camera moves, and **25-branch grid** lets different copies select independently. The current bake and transition use the rest pose.

The transition benchmark tests approach and retreat at seven orbit angles under two suns, checks the automatic whole-branch output against the forced representations, and renders all 16 regional masks against exact submission counts. The all-voxel regional result matches the whole-branch result, and both tints must appear in each selected mixed pose. The four regional masks contain 119, 103, 95, and 88 bricks, against 365 for the whole branch. On a 241-frame moving orbit, the whole branch switched twice and the regional selector made four smaller changes. The largest measured regional switch changed net resolved coverage by 7.3% and average display luminance on comparable foliage pixels by 1.9%. The regional selector had smaller local coverage differences than the whole-branch switch on this route. Review the transitions in motion for perceptibility.

| RTX 5090, 25 branches and 4× MSAA | Triangle-only GPU camera | Automatic GPU camera | Automatic foliage API calls |
| --- | ---: | ---: | ---: |
| Mixed across whole branches | 0.109 ms | 0.098 ms | 2 |
| Mixed within branches | 0.112 ms | 0.100 ms | 5–6 |
| Fully voxelized, whole-branch policy | 0.110 ms | 0.085 ms | 1 |

Timings are pooled medians across two runs at three orbit angles. Four to six copies had both representations active in the regional grid poses. Median regional mixed CPU render time rose from 0.088 to 0.095 ms, with selection rising from 0.002 to 0.004 ms. The results support a small GPU saving on this fixture. Shaded snapshots and CSV reports are under `build/phase1/oak-branch-mixed-final-*`.

### Region 3 bake A/B

Region 3 covers fixed groups 96–128 and participates in the first mixed cut on the moving orbit. Variant A uses the dominant-owner cells from the fitted whole-branch bake. Variant B starts from that mask and reruns the coverage fitter using only the exact source triangles in those groups. The eligible cells come from those same triangles, so the fit can add cells where the original region contributed geometry. The fitter uses eight isolated training views and keeps the 0.125-unit grid and 88-brick cap. A uses 88 bricks and 490 cells. B uses 87 bricks and 395 cells, with 240 cells shared between the two bakes. B overlaps zero occupied cells from other regions in this asset.

The CPU comparison uses 12 holdout views at 650–1,200 units and 64 samples per pixel. The mixed reference uses LOD 6 for the entire branch, and the mixed candidates swap only region 3. The other foliage, bark occlusion, and camera rays are shared. Resolved pixel error sums absolute coverage differences per pixel and normalizes by reference coverage.

| Comparison | Mean resolved pixel error, A → B | Mean signed coverage bias, A → B |
| --- | ---: | ---: |
| Isolated region vs. source triangles | 21.12% → 19.46% | +12.23% → −0.99% |
| Isolated region vs. LOD 6 | 21.90% → 19.75% | +13.15% → −0.17% |
| Mixed branch vs. LOD 6 | 4.53% → 4.42% | +2.64% → −0.20% |

In the mixed CPU comparison, gained coverage fell from 5.76% to 5.04% of the reference and lost coverage rose from 3.12% to 5.24%. The GPU comparison uses 4× MSAA and holds the camera at region 3’s entry and exit thresholds, across seven orbit angles and two suns. With the other regions on triangles, the mean absolute net coverage step fell from 3.67% to 2.45%. The mean resolved pixel difference rose from 7.58% to 7.94%. The ten additional captures with some voxel neighbors showed the same tradeoff. The GPU camera pass for 25 copies took 0.0996 ms with A and 0.0989 ms with B. Both used the same triangle shadow casters. Keep the shared-mask bake as the default. The isolated fit corrects density and yields a small change to the placement of foliage in the mixed cut.

From the repository root, use these commands for the CPU bake and the two GPU arms.

```powershell
.\build\phase1\Release\CompareVoxelFoliage.exe build/phase1/Release/assets build/phase1/oak-branch-region-ab --region-ab 3 --samples 8
.\build\phase1\Release\CustomGameEngine.exe --branch-region-benchmark build/phase1/oak-branch-region-ab/gpu-shared-a.csv 3
.\build\phase1\Release\CustomGameEngine.exe --branch-region-benchmark build/phase1/oak-branch-region-ab/gpu-independent-a.csv 3 build/phase1/oak-branch-region-ab/independent.voxels
```

The bake, metrics, and shaded captures reproduced on repeated runs. Saved results are under `build/phase1/oak-branch-region-ab`. Gain and loss maps are in the retreat, side, and rear 700-unit pose directories.

### Reuse the branch as a reference

- [x] **Fixed-group selector.** Select one level per group with all representations resident. Verify a mixed-detail cut and hold it across a camera move.
- [ ] **Runtime switching review.** Replay an approach, retreat, and orbit through the recorded poses, then repeat at another resolution or FOV. Compare against forced levels, log which groups switch and when, and use freeze cut to inspect the same selection from a new view. Compare camera coverage and ground shadows with the same representation in both passes.
- [ ] **Area compensation.** Measure the current all-leaf levels against the source with paired canopy masks and ground-shadow captures at several sun angles. Separate missing and gained coverage so a similar total cannot hide shifted gaps. If testing redistribution after whole-leaf removal, compare the same surviving leaves at the same triangle count with and without compensation. Use the all-leaf LOD 6 and the source as controls. Epic's Preserve Area expands open boundaries on the survivors. [Epic's foliage notes](https://dev.epicgames.com/documentation/unreal-engine/working-with-naniteenabled-content#foliage-using-nanite)
- [x] **Whole-branch frustum culling.** The **Frustum culling** toggle tests each branch against the camera and sun frusta independently. The bounds enclose original and fitted proxy vertices and expand by the frame's wind displacement. A frozen-cut GPU comparison found identical pixels in 49 paired renders at 3200 × 1800, including 1× and 4× sampling, source and forced geometry, both Automatic draw paths, and still and displaced foliage. A diagnostic that removed off-camera branches from shadow submission changed ground pixels in six cases. In one Automatic view at 35 scene units, 19 of 25 branches passed the camera test and camera foliage dropped from 909,001 to 650,350 triangles. All 25 passed the shadow test.
- [x] **Shadow LOD calibration.** Compare camera-matched shadows with forced shadow geometry on a frozen cut. Record draw work, shadow timing, and visible receiver changes with still foliage and wind. [Results](#shadow-lod-calibration)
- [x] **Offline binary voxel baker.** Verify coverage sampling and pack three Oak Branch cell sizes with a shared origin. [Results](#voxel-bake-phase-i)
- [x] **CPU voxel comparison.** Measure gained and lost coverage, openings, and nearest hit depth against source and LOD 6 before selecting forced GPU candidates. [Results](#cpu-voxel-comparison-phase-ii)
- [x] **Forced GPU voxel comparison.** Verify depth and coverage against the CPU tracer and measure the cost crossover with fixed triangle shadows. [Results](#gpu-voxel-comparison-phase-iii)
- [ ] **Further culling.** Test a solid occluder, finer visibility bounds, and a larger grove. Compare frame costs, especially the cost of building separate draw lists when the two visibility sets differ. [Culling follow-up](FUTURE_CONSIDERATIONS.md#rendering)
- [ ] **Voxel foliage.** Use the same source leaves and poses to compare a few forced voxel sizes with full-detail triangles and LOD 6 at distances where typical leaves are near or below a pixel. Compare the positions and sizes of gaps, silhouette, depth under an orbit, relighting, motion, and ground shadows. Test a whole crown next for density and parallax, then a grove for cost. [Epic's Nanite Voxels](https://dev.epicgames.com/documentation/unreal-engine/nanite-foliage#nanite-voxels)

Run the switching comparison first. Measure the current shadow changes on that route before testing compensation, and use the resulting triangle baseline for the voxel comparison.

## 1. Establish the source and baseline

- [ ] Use the existing **Oak Branch** scene (`--branch`), its exported GLB, and the documented Blender export path. Record the source fingerprint and verify the reference counts of 3,080 modeled leaves, 123,200 foliage triangles, and 9,888 bark triangles. Preserve the bark and foliage assignments, winding, normals, transforms, UVs, and two-sided foliage transmission. Draw bark at full detail for the first foliage comparison. [Oak Branch export and viewer controls](../README.md#checkpoint-12a-modeled-oak-branch)
- [ ] Use Overview, Close-up, and Under canopy. Save a backlit pose and a repeatable approach, retreat, and orbit that cross actual LOD transitions. Capture the full-detail reference at the same camera samples as each candidate.
- [ ] Start from the existing GLB and identify connected components from verified mesh topology. Assign nearby components to deterministic fixed groups for the first oak run. Treat Blender-authored component IDs or flat shoot labels as optional input if topology is ambiguous or spatial groups give poor results. Inspect group colors before baking LODs. Verify that UV or normal splits do not fragment a single source surface and that position welding does not join independent coincident surfaces. Preserve source topology IDs when available and report ambiguous seams if the GLB cannot resolve them.

## 2. Choose where the bake runs

- [ ] Put the offline C++ command-line baker under `CustomGameEngine/Tools/` and build it as a separate CMake target when implementation begins. The existing Python exporter lives in that same repository and runs inside Blender. The C++ baker runs on the exported data and builds the cluster hierarchy itself. Blender continues to provide source geometry and any optional flat labels. Reuse the engine's GLB geometry semantics so the baker and viewer agree on transforms, materials, and vertex attributes. Keep the engine's indexed draw path for the viewer experiment.
- [ ] Use the source GLB plus optional authoring metadata as input. For the first pass, plan a versioned companion bake with a small inspectable manifest and packed vertex/index payload. Include source fingerprint, recipe and library version, stable cluster and replacement IDs, draw ranges and material IDs, level, visibility bounds, and replacement membership in both directions. Preserve an independent full-detail reference. Keep the same source asset frame and wind inputs for all variants.
- [ ] Store **object-space errors** and shared projection bounds in the bake. Pixel error is computed per view from those inputs, the instance transform, and the viewport. Any offline pixel values are measurements for specified test views. Store the meaning and units of the error field so that a normalized simplifier score never gets interpreted as meters.
- [ ] Emit a bake report with per-level triangles, unique vertices, cluster occupancy, group sizes, shared edges cut, bounds extent, reductions achieved, and groups that stalled under the locks. Record settings, seed or tie-breaking policy, bake time, and output size. Verify stable results for repeated runs with identical input and configuration.

## 3. Build two comparable representations

### Fixed groups

- [ ] Give each group a full-detail representation and a small sequence of prebuilt coarser ones. Lock open borders and start by simplifying component interiors while retaining at least one nondegenerate triangle per input surface component. If a component reaches its useful reduction limit, keep its last valid representation. For the oak, inspect how curvature, orientations, and gaps change with the rim held fixed.
- [ ] Track source component IDs through the bake, including components with no surviving triangles. Count survival across a complete representation or cut, since one component can span multiple clusters. Report partial surface loss through the positional and coverage checks. Treat deliberate whole-component removal as a separate optional recipe after the first comparison. Keep its survival data and visual assessment explicit.
- [ ] Pack each representation into as many capped draw clusters as needed. One fixed group can occupy several clusters. Choose its whole representation together. Keep all levels resident and use the same selected representation in the camera and shadow passes for the first controlled comparison.

### Regrouped cluster DAG

- [ ] First partition the original foliage triangles and draw the complete clustered LOD0. Verify that every source triangle is assigned once and that materials, edge positions, and rendered coverage match the source. Measure this case before simplification so the cost of additional indexed draws is visible.
- [ ] Group current clusters, merge their triangles, lock the shared perimeter with neighboring groups, simplify, then repartition the result into capped clusters. Keep component deletion disabled in the initial DAG comparison too. Repeat while useful reduction is possible. Record each merge/split as one replacement event with a fine set and a coarse set. A coarse output may join a different group in the next round. Record which previously locked edges became interior.
- [ ] Keep topology used for adjacency distinct from the draw vertex attributes. Preserve material boundaries and protect relevant shading or UV seams. Track open surface borders separately from inter-group seams. Locking every open border can prevent useful reductions on thin surfaces. Keep border changes measurable, and require inter-group boundary positions and edge segmentation to match, including under wind. Spatial links between components can influence grouping while leaving their geometry disconnected.
- [ ] Use meshoptimizer's meshlet builders and `clusterlod.h` as the first library reference. Compare the library's allowed reduction against the actual boundary policy and the measured foliage quality. Keep the last valid representation when locks or quality limits stall a group. Allow a coarsest set of several clusters. [meshoptimizer documentation](https://github.com/zeux/meshoptimizer#readme) · [Cluster LOD builder](https://github.com/zeux/meshoptimizer/blob/master/demo/clusterlod.h)

## 4. Define the capped partitioning experiment

For triangle clustering and the split after simplification, build the mesh dual. Each triangle contributes one unit of node weight. Put a link between faces that share a geometric edge, with unit weight per shared edge. Minimize the sum of weights of links whose endpoints land in different parts. Each triangle must belong to exactly one part. Enforce the configured triangle cap and validate any vertex cap imposed by the chosen builder. For an experiment with cap `C` and `N` triangles, `ceil(N/C)` is a lower bound on the number of parts. Vertex limits, material boundaries, or a preference for tighter bounds can require more parts.

For grouping existing clusters, make the **cluster** the node and give each pair a link weighted by its number of shared triangle edges. Group whole nodes, using a target number of clusters or a triangle workload budget per group. Keep this budget separate from the cap on each output cluster. For disconnected components, favor local packing using spatial proximity, and track topological cut weight separately from any spatial penalty. Otherwise zero shared edges can make a geographically scattered partition look ideal.

- [ ] Start with the capped meshoptimizer builder. Record fill, triangle and vertex counts, bounds extent, disconnected components per cluster, and geometric cut weight. Use identical topology and caps when comparing another partitioner.
- [ ] If seam density or packing limits the result, compare METIS on both the branch and a connected fixture. METIS coarsens the weighted graph, partitions the smaller graph, then uncoarsens and refines with boundary moves. We control the graph, weights, and capacity checks. Its cut objective scores graph links, so judge spatial compactness and reduction separately. Consider custom region growing and local moves if the measurements show that a geometry-specific constraint, such as a maximum cluster extent, needs to enter the solver directly.
- [ ] For an over-cap part, consider a boundary move into a part with spare capacity. The cut-cost change is the weight to the old part excluding the moved node, minus the weight to the destination. If a part still violates the cap, split it and validate again. Use deterministic capacity-aware packing within the offending part as a fallback, permitting extra parts so the hard cap always wins. Track how often repair happens and what it costs. On a tiny graph, enumerate legal assignments to check both cut scoring and the cap.

For a hand check, take eight triangles in an edge-connected chain and set the cap to four. A 5–3 split cuts one link and violates the cap. Moving the boundary triangle from the larger part produces a 4–4 split and still cuts one link. A partition with four triangles in each part that alternates membership along the chain cuts all seven links. Use a second fixture with zero adjacency to check that deterministic spatial packing keeps nearby disconnected leaves together under the same cap.

[Epic’s speaker notes, *Initial clustering*, *Graph partitioning*, and *DAG: Which clusters to group?*](https://advances.realtimerendering.com/s2021/Karis_Nanite_SIGGRAPH_Advances_2021_final.pdf) · [METIS partitioning methods](https://github.com/KarypisLab/METIS)

## 5. Bake error and select a valid cut

Epic's 2021 notes describe quadric-driven edge collapses and a simplifier error estimate that they project to pixels using a shared sphere. Quadric cost estimates the squared deviation from surface planes and can include weighted attribute changes. The notes characterize the perceptual estimate as a heuristic. Our first selection metric will be an independently measured positional deviation, with the quadric score logged separately.

- [ ] Generate candidate levels with meshoptimizer's edge-collapse simplifier and explicit perimeter locks. Use its position and optional attribute costs to prioritize collapses. Record the library error with its units and settings. Track normal deviation separately to see when a geometrically close surface changes its lighting.
- [ ] For fixed groups, compare each coarse level directly to the full-detail source. Sample both surfaces at vertices, along borders, and across triangle interiors with deterministic spacing. For each sample, find the closest point on a **triangle** of the corresponding component on the other surface. Take the maximum of the two directed sample maxima as the initial object-space selection error. Log the two directions separately and verify sensitivity to sampling spacing. This is a sampled estimate of worst deviation. Restricting comparisons to matching components prevents a neighboring surface from serving as a substitute for a missing component. If a component disappears, record that event separately and report its source area. Evaluate deliberate deletions through their own coverage and gap measurements before using those levels in automatic mode.
- [ ] For the regrouped DAG, evaluate each complete replacement over the union of its immediate fine and coarse sets. Use matching component provenance where available and log coverage changes separately. For the initial accumulated estimate, add the local two-way deviation to the maximum inherited fine error. Sampled values remain estimates. That sum bounds accumulated displacement when the inputs are genuine bounds over matching domains. Where an original comparison domain is available, check the accumulated estimate against a direct source measurement as well.
- [ ] Give every replacement event one shared error and projection bound. Use zero positional error at full detail, promote coarse errors to at least the maximum of their finer inputs, and enclose the finer projection bounds. Keep the raw and promoted values in diagnostics. Monotonicity is required for the cut even when the underlying error is an estimate.
- [ ] Start with a CPU selector and two reference checks. First walk complete replacement events from the coarsest valid cut and commit each refinement as a set. Check that the full coarse side is present when applying an event. Then scan the cluster records and check the local rule `own projected error <= threshold < parent projected error`. Here “parent” is the shared error of the event that replaces this cluster with coarser geometry. Compare the two cuts across views and exact threshold ties. Treat a root's parent error as infinite. Use a consistent per-view projection and verify that projected errors are nondecreasing toward the root across every dependency.
- [ ] For a center-view sanity check, compute `focalPixels = viewportHeight / (2 × tan(verticalFov / 2))` and estimate `pixelError = focalPixels × worldError / nearestBoundDepth`. Use an off-axis-aware conservative projection for automatic selection, and prefer finer detail for bounds crossing the camera or near plane. Verify scale and FOV behavior by hand. Start with a tunable 1 px threshold for observation, then use the foliage comparisons to choose a useful target.
- [ ] Run selection with every representation resident and submit the complete cut through the indexed renderer. Record selection and submission costs. Cluster visibility culling and BVH traversal are deferred to [Future considerations](FUTURE_CONSIDERATIONS.md#rendering).

One first projection option is to use a camera-space box that contains the positional comparisons and encloses the finer projection boxes. For a perspective camera, let `zMin` be its nearest positive depth and `rMax` an upper bound on the sideways distance from the optical axis. Use `focalPixels × worldError / zMin × sqrt(1 + (rMax / zMin)^2)`. This bounds the projection magnification along displacements inside the box. Pad the box to enclose those displacements, refine nonzero-error representations when the bound reaches the near plane, and leave full detail at zero error. The arithmetic bounds the projection of the supplied displacement. Its accuracy against the original still depends on how the displacement was measured.

[Epic’s speaker notes, *Mesh simplify*, *Error metric*, *LOD selection*, and *LOD selection in parallel*](https://advances.realtimerendering.com/s2021/Karis_Nanite_SIGGRAPH_Advances_2021_final.pdf)

## 6. Give the Scene Inspector controls for diagnosis

- [ ] Add views for source full detail, clustered full detail, forced shoot level, forced baked DAG cut, and automatic pixel error. Support explicit coarse and intermediate cuts saved from the bake or built by complete replacements. On an uneven hierarchy, forcing a numeric level must still produce a complete cut. Show each inspected cluster's ID, group, actual level, triangle count, and the shared own and parent errors in object units and pixels.
- [ ] Add a threshold slider and a freeze-cut toggle that lets the camera keep moving. Freeze the selected IDs and continue to display how their projected errors change as the camera moves. Inspect a selected shoot, cluster, or complete replacement in isolation. Label isolation as a partial view so intentional omissions are easy to distinguish from a broken cut.
- [ ] Provide a normal shaded mode and unlit overlays for **triangle**, **cluster**, **fixed/replacement group**, and **LOD level**, plus projection bounds and locked edges. Use a deterministic palette keyed by stable IDs. For a triangle-flat tint, use a per-primitive ID with a draw-local-to-baked ID mapping, or an equivalent debug-only geometry path. Ordinary interpolated vertex colors would blend across a triangle. Distinguish retained source IDs from newly generated triangles where lineage is available. Keep tint modes out of performance captures.
- [ ] Keep the current wind pause, sun, and MSAA controls available. Use a solid foliage coverage view and a light-view or receiver-shadow comparison to check gaps separately from shading. Preserve the original material path for the final image comparison.

## 7. Record results and decide whether to advance

| Question | Record on the same route |
|---|---|
| Does the data work? | Every LOD0 triangle assigned once, no cap violations, complete replacements with matching shared edges, valid monotonic cut, and matching seam deformation under wind. Exercise the earlier two-round strip, a thin curved surface, and separated components where deleting one would be easy to miss in a union distance test. |
| Is there a useful reduction? | Camera and shadow triangles and draw counts, selected clusters, CPU selection/submission time, GPU camera/shadow time, and total GPU time. Compare the source, unsimplified clusters, fixed groups, and regrouped clusters at matched quality and at matched triangle budgets. |
| Where does foliage disappear or fill in? | Compare foliage coverage fraction by tile, with missing and gained sample coverage counted separately at fixed MSAA. Compare gap locations and sizes, outline and leaf-tip displacement, first visible leaf depth, and ground shadow coverage. |
| What changes in motion? | Inspect frame-aligned approach, retreat, and orbit clips for changes in coverage and shading relative to the full-detail clips. Log switches and inspect the worst frames. Repeat with zero wind and with a reproducible wind sequence. |
| What did regrouping buy? | Compare locked-edge density, fill, cluster bounds, useful reduction at the same error target, and measured render and bake costs. |

- [ ] Record resolution, FOV, GPU, build, lighting, wind, MSAA, recipe, and threshold with each capture. Use warmed-up runs and median plus slow-frame timings. The branch is the primary quality fixture. Use repeated branches later if a single branch's timing is too small to distinguish from noise.
- [ ] Require the structural checks to pass. Agree on practical visual tolerances after capturing the full-detail baseline and before grading the competing recipes. Record the worst acceptable forced cut at each test view and compare it to the automatic selection. Keep the simplest representation that preserves the observed silhouette, openings, lighting, and motion at a useful reduction. Advance regrouping if it measurably improves those results or reduces a diagnosed seam or packing cost.

## Decisions for the first design pass

- [ ] Verify the [agreed topology and lock policy](OAK_BRANCH_LOD_ASSUMPTIONS.md) in the first bake. Check component IDs against the branch export, confirm that open outlines survive interior simplification, and check shared boundary positions and segmentation on the connected fixture before regrouping.
- [ ] Pick the first comparison. Bake a full-detail level and two coarser fixed-group levels with all components retained. Use configurable budgets, with 128 triangles as a starting cluster cap when testing packing. Report achieved reductions and which constraint stopped each simplification. Use the forced cuts on the existing camera presets to choose the first usable error threshold.
- [ ] Agree on the fidelity test for the selector. Compare its cut with the forced cuts that pass the silhouette, gap, and shadow checks. Log oscillation around the initial pixel threshold. If that causes visible popping, discuss hysteresis at complete replacement decisions before adding transition behavior.
