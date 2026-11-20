# Exposed white oak bough voxel results

**Keep the reusable triangle cut for bough 7 and hold the interior experiment.** At 100 m the triangle control passed the Preserve Area silhouette gates at all 13 angles. Replacing one 12-spray group saved 924 submitted triangles, passed seven angles, and increased isolated camera time in every paired run. Replacing the entire bough gave a larger count reduction but missed the quality target at all 13 angles. The 24–41 m whole-crown triangle-efficiency question remains open. [Review image](../build/oak-bough-voxel/comparison.png) · [Per-view validation](../build/oak-bough-voxel/validation.csv) · [Future considerations](FUTURE_CONSIDERATIONS.md)

## Where Nanite changes representation

The native fixture has Voxelize enabled on both the two-material spray and its 371-placement assembly. The six GPU readbacks agree with Unreal’s on-screen counts of selected clusters, surface triangles and bricks. The percent column measures first-visible voxel pixels among Nanite pixels in the isolated bough. The edge triplets are p10 / median / p90 weighted by occupied cells. World edge is the cube root of transformed cell volume. For projected edge we use the longest transverse projection of the three transformed edges at each brick’s depth. Local and world edges agree to the precision shown on this fixture. [Raw cut statistics](../build/oak-bough-voxel/native-cuts.csv) · [HUD check](../build/oak-bough-voxel/counters.png) · [Decoder](../Tools/DecodeOakBoughVoxels.py)

| Distance | Surface triangles | Bricks | Occupied cells | Visible voxel pixels | Local/world edge in cm | Projected edge in px |
|---|---:|---:|---:|---:|---|---|
| 41 m | 143,752 | 0 | 0 | 0% | — | — |
| 50 m | 93,618 | 0 | 0 | 0% | — | — |
| 52 m | 93,366 | 59 | 157 | 0.055% | 2.07 / 2.07 / 2.07 | 0.87 / 0.87 / 0.88 |
| 56 m | 55,062 | 9,027 | 24,021 | 23.26% | 2.07 / 2.07 / 2.07 | 0.90 / 0.95 / 0.98 |
| 58 m | 11,466 | 19,234 | 51,182 | 79.35% | 2.07 / 2.07 / 2.07 | 0.88 / 0.94 / 0.96 |
| 100 m | 378 | 11,767 | 30,737 | 99.94% | 2.76 / 3.04 / 3.04 | 0.72 / 0.80 / 0.83 |

At 52, 56 and 58 m the same 59-brick part cluster appears on 1, 153 and 326 instances respectively. At 100 m all 99 selected voxel clusters are in assembly space. No selected voxel cluster carries a spray instance transform. The remaining three clusters contain triangles. This supports a reusable spray candidate through the transition and merged candidates farther away. [100 m selected clusters](../build/oak-bough-voxel/clusters-100.csv) · [Native brick packing](../../UnrealEngine/Engine/Source/Developer/NaniteBuilder/Private/Encode/NaniteEncode.cpp) · [Native voxel rasterizer](../../UnrealEngine/Engine/Shaders/Private/Nanite/NaniteRasterizer.usf)

The D3D replay uses the selected brick masks, orientations, material slots, decoded normal axes and residual triangles. Its 100 m mask reached **99.34% IoU against the native selected mask**. Of 12,567 shared camera depth samples, absolute error was 0.40 mm at the median, 0.51 mm at p95 and 20.43 mm at p99. Unreal classified 12,560 pixels as voxels and the D3D replay classified 12,640. Both rasterizers use midpoint depth for the first occupied cell. D3D uses indexed tight boxes and its own material lighting. [Native depth check](../build/oak-bough-voxel/replay-depth.json) · [D3D rasterizer](../shaders/Voxel.hlsl)

## Matched camera and cost

All D3D rows use the RTX 5090, 3840 × 2160, a 45° vertical FOV, 1× sampling, frozen wind, direct hierarchy submission, frustum culling, HZB off and the same triangle shadow casters. Leaves are two-sided and isolated twig triangles are one-sided. Each time is the median of four 61-frame run medians with alternating run order. The last column compares each 132° mask to the finest D3D source. The native Preserve Area gate at this angle is **IoU ≥ 0.8881 and outline error ≤ 4.103%**. Outline error counts exterior and hole edges more than one pixel apart in either direction. [Paired camera, shadow and frame times with spreads](../build/oak-bough-voxel/cost.csv) · [Per-run results](../build/oak-bough-voxel/timings.csv)

| Isolated cut at 100 m | Surface triangles | Bricks | D3D submitted triangles | Shaded camera ms | IoU / outline error |
|---|---:|---:|---:|---:|---:|
| Current reusable triangles | 74,047 | 0 | 74,047 | 0.07973 | 0.9030 / 3.49% |
| Nanite Preserve Area replay | 58,823 | 0 | 58,823 | 0.07546 | 0.8887 / 4.08% |
| Nanite Voxelize replay | 378 | 11,767 | 141,582 | 0.07864 | 0.7036 / 9.54% |
| Fitted 3.8 cm leaves, control twigs | 34,384 | 3,299 | 73,972 | 0.07741 | 0.7809 / 12.68% |
| Fitted 3.8 cm leaves and twigs | 0 | 4,625 | 55,500 | 0.07862 | 0.7660 / 13.27% |
| Uncapped 1.9 cm leaf redistribution, control twigs | 34,384 | 26,365 | 350,764 | 0.09306 | 0.7659 / 8.31% |
| One 12-spray group at 5.8 cm, triangles elsewhere | 72,403 | 60 | 73,123 | 0.08275 | 0.8979 / 3.86% |

The 12 triangles per box apply to our D3D submission. Native Unreal reports bricks in its software voxel rasterizer. Its native Voxelize mask at 132° reached 0.7013 IoU and 9.59% outline error against its own finest mesh. Across four native runs, visibility plus Nanite base pass took **0.41835 ms** (0.4035–0.4234 ms). That scope includes Unreal's own visibility and material passes. The Preserve Area replay matched the native selected mask at 99.964% IoU. Both D3D replays isolate geometry choice under one renderer. [Unreal profile components](../build/oak-bough-voxel/native-profile.csv) · [Work counts, visible voxel pixels and buffer sizes](../build/oak-bough-voxel/work.csv)

In the crown, the partial cut and triangle control produced identical masks at 132°. The partial cut drew 2,241,922 submitted triangles and took **0.53755 ms**, compared with **2,242,846 and 0.53363 ms**. The fitted joint cut drew 2,224,299 and took 0.53101 ms. Crown outline error rose from 5.98% to 6.45% and that timing range overlapped the control's. [Crown images and counts](../build/oak-bough-voxel/crown.csv) · [Paired timings](../build/oak-bough-voxel/cost.csv)

## Why the savings fell short

1. **Which cells survive matters as much as cell edge.** We sampled source coverage at 1.9, 2.9, 3.8 and 5.8 cm, for both leaves and wood. At exactly 3,305 leaf bricks and 2.9 cm, projected-mask fitting raised IoU from 0.534 to 0.734 over the coverage-transfer arm. It retained 32,539 cells versus 14,699. The fitter could change cells within the same chosen brick locations. All seven training yaws used a single pixel-center ray per pixel. Six further yaws, including 17° and 204°, were held out. At 3.8 cm the fitted joint cut had 1,472 unmatched reference edges versus 214 unmatched candidate edges at 132°. Missing fine features dominated. [Size and occupancy sweep](../build/oak-bough-voxel/screen.csv) · [Binary fitter](../Tools/VoxelCoverageOptimizer.cpp) · [Projection and packing](../Tools/FitOakBoughVoxels.py)
2. **A filled pixel can belong to the wrong surface.** Against finest visible material IDs, the triangle control achieved leaf/twig IoUs of 0.881/0.617. Fitted joint voxels achieved 0.703/0.205. Their p95 depth error on shared foreground pixels was 2.59 m, compared with 1.00 m for the triangles. Rays through a lost leaf or twig can hit foliage metres behind it. The average leaf brightness change was below one display-code value at this pose. No custom finalist changed off-geometry shadow pixels by more than one code value at 25° sun elevation with triangle shadow casters. [Material, first-hit depth, lighting and shadow results](../build/oak-bough-voxel/depth-material-shadow.csv)
3. **Small replacements pay for whole draws.** We selected the deepest full 12-placement batch along the benchmark view before screening it. Its 60-brick replacement covered only 97 visible pixels and issued two voxel-material draws. Total camera draw calls rose from 25 to 29. The two-material payload was 7,632 bytes, with an estimated 384 KiB of committed D3D upload buffers after per-resource alignment. The allocation estimate covers the three upload buffers per material. The partial cut passed all 13 native Voxelize gates and seven strict triangle gates. The 3.8 cm joint cut passed two Voxelize gates and zero strict gates. [Memory and submitted work](../build/oak-bough-voxel/work.csv) · [Held-out images](../build/oak-bough-voxel/validation.csv)

The motion test forced the partial candidate wherever the selected hierarchy visited that group. Each outward or return leg had 60 frames from 41 to 100 m or back. With wind, worst outward IoU went from 0.9006 for triangles to 0.8945 for the partial cut. The p95 fraction of frame-to-frame changed pixels that disagreed with the source motion went from 14.91% to 15.65%. Frozen-wind results moved in the same direction. This tests a forced candidate. A production distance threshold has not been qualified. [Motion summary](../build/oak-bough-voxel/motion-summary.csv) · [Per-frame measurements](../build/oak-bough-voxel/motion.csv)

**Next priorities.** First let the fitter exchange brick locations using held-out outline, visible-material and first-hit-depth errors. Preserve geometric twig tips or leaf edges where a voxel candidate fails. Next pack accepted groups into shared per-material buffers and measure draw count and box overdraw before changing the rasterizer. Use the native mixed replay as the control for that renderer work. Reopen the interior experiment once an exposed group passes every established angle and shows a repeatable camera saving in the crown.

## Build and view

From the engine checkout with the existing oak benchmark fixtures, run the following in PowerShell. The script builds Release, stages both renderers' selected geometry for the D3D fixture, and renders a 100 m coverage comparison.

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\Tools\BuildOakBoughVoxels.ps1
Invoke-Item .\build\oak-bough-voxel\comparison.png
```

Add `-Refit -Benchmark` to rerun the custom sampling, qualification and timing suite. Add `-CaptureNative` to rebuild and capture the voxel-enabled fixture in the installed Unreal Editor using the existing benchmark project. The coverage-transfer arm uses our directional samples and representative normals with Epic's lowest-coverage-first neighbor transfer and overlap rule. Epic's native control uses its full hierarchy, ray distribution, normal-distribution fit and brick placement. The experimental camera sidecar is opt-in. [Build script](../Tools/BuildOakBoughVoxels.ps1) · [Custom bake](../Tools/BakeOakBoughVoxels.cpp) · [Epic coverage transfer](../../UnrealEngine/Engine/Source/Developer/NaniteBuilder/Private/Cluster.cpp) · [Group reduction](../../UnrealEngine/Engine/Source/Developer/NaniteBuilder/Private/ClusterDAG.cpp)
