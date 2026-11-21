# Equal-error triangle and voxel comparisons for four white oaks

## Results inside Unreal

| Native comparison | 50–100 m | 100–175 m |
|---|---:|---:|
| Default Voxelize | 0.573 ms | 0.490 ms |
| Closest median triangle setting | 0.572 ms at 2.25 | 0.483 ms at 1.25 |
| Coarsest constant triangles passing every primary pose and transition | 0.603 ms at 1.5 | 0.519 ms at 0.95 |
| Triangles with the original fitted distance rule | 0.574 ms | 0.491 ms |

The closest median settings passed only 28/52 and 24/52 spatial checks, respectively. Across both clips the fitted rule `min(2.25, 156 / distance_m)` passed all 104 spatial and 100 temporal checks on the primary yaw. Its GPU times overlapped the default voxel times. On yaw 23° it passed only 16/52 and 50/52 spatial checks, while passing all temporal checks. The near clip's failure came from displaced outline detail even though the median foreground IoU exceeded that of default voxels.

A separate comparison paired independently tuned voxel and triangle builds where all three median quality metrics agreed within half a percentage point. Six alternating-order timing pairs put triangles ahead each time. For 50–100 m, triangles at 1.5 took 0.603 ms and Voxelize at 0.65 took 0.671 ms, a 10.2% triangle saving with a maximum metric gap of 0.42 percentage points. For 100–175 m, triangles at 1.25 took 0.482 ms and Voxelize at 0.5 took 0.517 ms, a 6.7% saving with a maximum gap of 0.29 points. These are clip-level matches. Each pose can still differ. [Sweep curves](../build/oak-nanite-quality-sweep/curves.csv) · [Alternating-order pairs](../build/oak-nanite-quality-sweep/overlap-runs.csv)

The closest fixed settings agree directly with default voxels on a median 90.44% and 89.34% occupancy IoU. The image below uses native-size crops selected for high outline density. Both approaches preserve the broad crown shape, but gaps and fine branches occupy different pixels.

In the lit captures the closest triangle and default voxel settings differed by a median 26.0 and 29.7 code values per channel on a 0–255 scale within the union of the three canopy masks. Against the fine lit image, triangle errors were 18.7 and 22.9, and voxel errors were 24.9 and 30.5, on the same pixels. These are fixed-exposure RGB differences under this light rig. [Per-frame shaded differences](../build/oak-nanite-quality-sweep/shaded.csv)

Repeating the same native voxel render on the same mask produced 11.8 and 16.3 mean absolute RGB differences. Averaging each 4 × 4 pixel block before comparing reduced the triangle-to-voxel differences to 10.3 and 10.2, versus 4.7 and 5.8 for the repeated voxel renders. The occupancy masks were repeatable. Shaded RGB includes run-to-run variation. [Shaded repeat control](../build/oak-nanite-quality-sweep/color-control.csv)

![Native Unreal fine reference, tuned triangles, default voxels, and their coverage differences at 100 and 175 metres](../build/oak-nanite-quality-sweep/grove-crops.png)

## Three-view test against default voxels

After measuring the yaw 23° outline misses, the more conservative rule used `min(2.0, 140 / distance_m)`. Both budgets refer to Nanite's pixels-per-edge control. A higher number allows a coarser cut. Yaw 197° was held out when choosing the more conservative rule. In six alternating-order runs on the primary view, this rule took 0.570 ms versus 0.565 ms for voxels at 50–100 m, with overlapping run times. At 100–175 m it took 0.499 ms versus 0.475 ms for voxels. Voxels were faster in every far paired run. [Paired GPU measurements](../build/oak-nanite-quality-sweep/guard-gpu.csv)

| Range | Yaw 132° spatial / motion passes | Yaw 23° | Held-out yaw 197° |
|---|---:|---:|---:|
| 50–100 m | 52/52 · 50/50 | 52/52 · 50/50 | 44/52 · 44/50 |
| 100–175 m | 52/52 · 50/50 | 52/52 · 50/50 | 52/52 · 50/50 |

The held-out near misses occurred at 50, 52, 54 and 56 m in both directions. The worst outline excess was 1.68 percentage points. Median triangle counts with the conservative rule were 1.824M and 1.528M. Native default Voxelize submitted 0.704M triangles plus 848.5k bricks and 0.136M triangles plus 386.1k bricks. For the independently matched high-quality pairs above, the near triangle build submitted 3.953M triangles versus 6.459M triangles plus 88.1k bricks in Voxelize. The far pair was 1.031M triangles versus 0.812M triangles plus 981.3k bricks. Each run cost includes the full selected mixture. [Three-view quality](../build/oak-nanite-quality-sweep/guard-quality.csv) · [Native work counts](../build/oak-nanite-quality-sweep/guard-counts.csv)

## Our triangle renderer

The same fitted rule produced 1.582M and 1.254M median camera triangles on our existing GPU traversal. Tree GPU times were 0.501 and 0.376 ms, passing 52/52 silhouette checks and 50/50 transitions in each primary clip. For comparison, fresh default native voxel medians were 0.573 and 0.490 ms. This demonstrates a camera for which our triangles beat that native tree-pass time. The result applies to the measured camera, workload and gate. The capture-only option replays the fitted budget without changing the viewer's default triangle policy. [Counts, timings and error](../build/oak-nanite-quality-sweep/custom.csv)

Replaying the more conservative rule on our renderer cost 0.537 and 0.419 ms on the primary camera. It passed all 104 silhouette checks and 100 motion checks. The additional two views tested representation quality inside Unreal. Testing cameras with independent motion, wind, shadows and production anti-aliasing would be needed to extend this conclusion to gameplay. [Conservative D3D replay](../build/oak-nanite-quality-sweep/custom-guard.csv)

## Experiment

Voxel bricks give a dense group of disconnected leaves another way to preserve its apparent volume as detail is reduced. Unreal's 5.8 builder tries a voxel parent while the candidate cell size is below its triangle simplification error and checks the voxel and brick budgets. At runtime its brick rasterizer can reject covered pixels before tracing occupancy. These decisions use local geometric error and work estimates. Measuring camera images and time adds information about the actual cost-quality tradeoff for this oak. [UE 5.8 builder](<C:/Program Files/Epic Games/UE_5.8/Engine/Source/Developer/NaniteBuilder/Private/ClusterDAG.cpp#L1513>) · [UE 5.8 brick rasterizer](<C:/Program Files/Epic Games/UE_5.8/Engine/Shaders/Private/Nanite/NaniteRasterizer.usf#L1678>) · [Epic's foliage rendering overview](https://dev.epicgames.com/documentation/en-us/unreal-engine/nanite-foliage)

Unreal Engine 5.8 rendered the same four transformed white oaks with the same camera, lighting and source geometry. One build used Nanite Preserve Area triangles. The other used Nanite Voxelize, which selects a mix of triangles and bricks. Each 4K, single-sample clip has 26 poses outward and the same 26 inward. The primary yaw is 132°, the vertical field of view is 45°, wind is frozen, and HZB, anti-aliasing, motion blur, Lumen and shadows are off. Profiles exclude mask visualization, HUD and PNG readback. The hardware was an RTX 5090. [Capture rig](../Tools/CaptureMixedOakMotion.py) · [Sweep and analysis](../Tools/SweepOakNaniteQuality.py)

The independent sweeps varied `r.Nanite.MaxPixelsPerEdge` from 0.85 to 3.5 across 12 triangle settings and 0.5 to 1.4 across six voxel-enabled settings. Each ran on both camera ranges. The image reference was the finest 0.001 triangle capture. Repeating the default voxel capture reproduced its earlier masks on every primary-camera frame. [Quality and timing curves](../build/oak-nanite-quality-sweep/curves.csv)

Three error measurements track different failures. Foreground IoU measures agreement on filled pixels. Outline error measures the fraction of edge pixels farther than one axial pixel from the other outline, counting both directions. Motion error compares the pixels that changed on adjacent frames with the pixels that changed in the fine capture, normalized by their foreground union. Each camera leg has 25 transitions. A setting qualifies against default voxels only when its IoU is at least as high and both errors are at most as high on every applicable sample. Direct candidate-to-voxel IoU and lit-image differences separately measure how closely the two representations resemble each other. [Frame measurements](../build/oak-nanite-quality-sweep/sweep-frames.csv)

Two fixed triangle settings per clip were selected from the sweep. The closest setting minimizes the largest difference between the three median quality measures. The qualified setting is the coarsest tested constant that passes every silhouette and motion check. The distance-dependent setting was fitted to even-numbered outward poses at yaw 132°, with a margin on interpolated error. Unreal then rendered the actual changing cuts on both legs and on a second yaw of 23°. This range rule applies to this asset, camera and resolution. The test also replays the rule using our GPU triangle traversal. [Selected settings](../build/oak-nanite-quality-sweep/choices.json)

Native GPU times sum Nanite visibility and base pass. The sweep used two passes in opposite setting order. The shortlisted settings were profiled in six repeated blocks. The separate overlap comparisons reversed the order of the triangle and voxel arms on alternating repeats. Reported times are medians of the run medians. The D3D tree GPU timestamp includes its GPU selection, indirect draws and forward shading. Cross-engine timings include these pipeline differences. [Native repeated runs](../build/oak-nanite-quality-sweep/paired-runs.csv) · [Close-quality pairs](../build/oak-nanite-quality-sweep/overlap-runs.csv) · [D3D replay](../build/oak-nanite-quality-sweep/custom.csv)

The confirmation and three-view editor processes returned a shutdown access violation after recording completion and closing their logs. The capture driver verified fresh images and profiles for every requested job, and scoring checked the expected profile lengths.

## Reproduce the comparison

Use the staged UE project and the Python environment containing NumPy and Pillow. Build the Release viewer before the optional D3D replay. From the repository root run the following commands in order.

```powershell
python Tools/SweepOakNaniteQuality.py sweep
python Tools/SweepOakNaniteQuality.py score
python Tools/SweepOakNaniteQuality.py plan
python Tools/SweepOakNaniteQuality.py confirm
python Tools/SweepOakNaniteQuality.py report
python Tools/SweepOakNaniteQuality.py overlap
python Tools/SweepOakNaniteQuality.py illustrate
python Tools/SweepOakNaniteQuality.py custom
python Tools/SweepOakNaniteQuality.py custom-report
python Tools/SweepOakNaniteQuality.py guard
python Tools/SweepOakNaniteQuality.py guard-report
python Tools/SweepOakNaniteQuality.py color-control
python Tools/SweepOakNaniteQuality.py custom-guard
```
