# Forked white oak comparison

Unreal Engine 5.8.4 (CL 58902052) and the custom engine Release build ran on the same RTX 5090 at 1440 × 1260. All runs used the exported oak's trunk and 4,730 saved PVE spray transforms. The measured fields of view were 45° vertical in the custom capture and 50.6645° horizontal in the Unreal capture. Wind and temporal anti-aliasing were disabled. The custom engine used 1× MSAA and its existing lighting. Unreal used a fixed key and fill light with Lumen diffuse indirect lighting and reflections disabled.

The Unreal staging project contains four native static-mesh assemblies. Each has one reusable spray part. The original spray has 225,990 leaf and 15,240 wood triangles. The pre-reduced spray has 1,504 leaf and 950 wood triangles, extracted from the existing bakes with their UVs and normals. Both inputs used the same Unreal leaf and bark materials. Each input was built with **Preserve Area** and **Voxelize** on the spray and tree. This preserved placement, scale, and orientation across variants. [Verified assembly settings](../build/unreal-nanite-oak/verified-assemblies.json) · [Matched lit views](../build/unreal-nanite-oak/matched-views.png)

## Where Nanite changes representation

Frame coverage is the percentage of all image pixels occupied by the oak. Voxel share is the percentage of those oak pixels colored green by Nanite's Voxels visualization. These are camera measurements from the offscreen target. Once warmed, the direction of travel yielded the same transition brackets to the 2 m sampling resolution. [Representation sheet](../build/unreal-nanite-oak/nanite-cuts.png) · [Camera sweep video](../build/unreal-nanite-oak/oak-camera-sweep.mp4) · [Sweep samples](../build/unreal-nanite-oak/nanite-sweep.csv)

| Quarter view | Custom frame coverage | Original voxel build frame coverage | Original voxel share | Optimized voxel build frame coverage | Optimized voxel share |
| ---: | ---: | ---: | ---: | ---: | ---: |
| 14 m | 74.18% | 74.07% | 0.02% | 73.92% | 0.00% |
| 18 m | 52.34% | 51.57% | 1.73% | 51.89% | 0.00% |
| 24 m | 28.85% | 27.39% | 61.77% | 28.27% | 0.00% |
| 41 m | 9.46% | 9.08% | 82.95% | 8.90% | 81.16% |
| 100 m | 1.57% | 1.50% | 90.94% | 1.50% | 90.94% |

In the 10–100 m sweep, the first pose at or above 1% voxel share fell between **16–18 m** for the original spray and **28–30 m** for the optimized spray. The visible tree became majority voxel between **22–24 m** for the original and **34–36 m** for the optimized spray. With the rotating sweep, these majority brackets correspond to roughly **33–27%** and **13–12%** frame coverage respectively. Preserve Area variants had zero voxel pixels. The first two retreat poses for each variant were recaptured after a streaming hold because some pages were unavailable during the initial frames. The quick captures and separate warmed captures are both saved.

At a fixed 18 m pose, changing the budget changed the cut without changing the asset. Both moving the threshold from 1 to 2 and halving the render dimensions from 1440 × 1260 to 720 × 630 reduced the demand for each screen-space edge detail by roughly the same amount. [Full coverage measurements](../build/unreal-nanite-oak/nanite-coverage.csv)

| Offscreen setting at 18 m | Original voxel share | Optimized voxel share |
| --- | ---: | ---: |
| 0.5 px at 1440 × 1260 | 0.00% | 0.00% |
| 1 px at 1440 × 1260 | 1.73% | 0.00% |
| 2 px at 1440 × 1260 | 82.40% | 16.02% |
| 1 px at 720 × 630 | 82.42% | 16.00% |

## Error settings and build rules

| Setting | Tested value | Role |
| --- | --- | --- |
| `r.Nanite.MaxPixelsPerEdge` | 1.0 by default, tested at 0.5 and 2.0 | Nanite scales the per-view projection by the reciprocal of this value. Hierarchy traversal tests projected parent and child error bounds against that scale. |
| `r.Nanite.MinPixelsPerEdgeHW` | 32.0 | Threshold in the hardware versus software rasterizer dispatch logic. |
| `r.Nanite.PrimaryRaster.TimeBudgetMs`, `r.Nanite.ShadowRaster.TimeBudgetMs` | 0.0 for both | Dynamic raster quality throttling by elapsed time was inactive. |
| Nanite streaming pool | 512 MiB with quality scaling limits at 70% and 85% | Residency and pool pressure can affect the available cut. |
| Keep Triangle Percent, Trim Relative Error, Max Edge Length Factor | 100%, 0, 0 on every build | The build accepted the full source input and used its internal hierarchy. |
| Voxel Level, Num Rays, SGGX distribution, Voxel Opacity | 0, 64, on, off | Voxel candidates were chosen by the normal group builder. |
| Virtual shadow maps | Directional resolution LOD bias −1.5, 4,096 physical pages | The cache was disabled for GPU profiling and enabled for visual captures. The light view has its own projected cut. |

During the build, `FClusterDAG::ReduceGroup` compares a candidate voxel cell size against the error of triangle simplification. The voxel candidate must fit the occupied-cell and brick budgets. The builder keeps coarser error bounds at least as large as their children's bounds. At runtime, the projected hierarchy can therefore draw triangles and voxels in the same view, and the result can change with pose, resolution, and residency. These tests establish the observed ranges for this asset and projection. [Live CVar values](../build/unreal-nanite-oak/unreal/runtime-settings.json) · [Local Nanite source review](NANITE_FOLIAGE_PIPELINE_REVIEW.md) · [Epic's Nanite Foliage documentation](https://dev.epicgames.com/documentation/unreal-engine/nanite-foliage?application_version=5.8)

## Geometry and timing

The native Nanite counters come from the editor viewport. It showed 38.27% and 0.68% voxel share at 18 m for the original and optimized builds respectively, compared with 1.73% and 0.00% in the offscreen target. Keep the viewport triangle, cluster, and brick counts with the viewport representation percentages recorded on the same path. The custom engine counts triangles submitted to its camera pass. [Native counter captures](../build/unreal-nanite-oak/native-viewport-counters.png) · [Viewport coverage and counts](../build/unreal-nanite-oak/nanite-viewport-coverage.csv) · [Hierarchy views](../build/unreal-nanite-oak/matched-groups.png)

| Variant | 41 m camera triangles | 41 m Nanite clusters / bricks | 100 m camera triangles | 100 m Nanite clusters / bricks |
| --- | ---: | ---: | ---: | ---: |
| Unreal original triangles | 1,674,601 | 13,332 / 0 | 169,087 | 2,247 / 0 |
| Unreal original voxels | 103,132 | 2,686 / 221,177 | 5,162 | 377 / 40,026 |
| Unreal optimized triangles | 1,600,416 | 12,730 / 0 | 165,900 | 2,168 / 0 |
| Unreal optimized voxels | 88,480 | 3,077 / 202,656 | 6,550 | 401 / 41,185 |
| Custom grouped | 3,227,340 | — | 3,227,340 | — |
| Custom bough proxy | 3,227,340 | — | 3,138,130 | — |

GPU entries below are per-frame medians. The Unreal values come from the second runs and the custom engine values use 51 timed frames per pose. Unreal camera visibility and base-pass costs are reported separately. Shadow Depths is the Unreal VSM scope. The custom Camera and Shadow columns are inclusive timestamps for its respective passes. The timing scopes, visibility stages, and shadow systems differ across renderers. [Unreal GPU samples](../build/unreal-nanite-oak/nanite-timings.csv) · [Custom engine GPU samples and culling counters](../build/unreal-nanite-oak/custom-timings-and-coverage.csv)

| Variant | 18 m camera visibility / base, ms | 18 m shadow, ms | 41 m camera visibility / base, ms | 41 m shadow, ms |
| --- | ---: | ---: | ---: | ---: |
| Unreal original triangles | 0.432 / 0.143 | 1.248 | 0.319 / 0.074 | 0.887 |
| Unreal original voxels | 0.428 / 0.144 | 1.313 | 0.335 / 0.069 | 0.967 |
| Unreal optimized triangles | 0.401 / 0.141 | 1.207 | 0.314 / 0.074 | 0.862 |
| Unreal optimized voxels | 0.396 / 0.141 | 1.314 | 0.316 / 0.067 | 0.948 |
| Custom grouped | 1.291 | 0.241 | 0.327 | 0.241 |
| Custom bough proxy | 1.289 | 0.241 | 0.327 | 0.240 |

Pre-reducing the spray removed **98.98% of the imported triangles**. On the voxel build it reduced Nanite's 18 m visibility-buffer median by 0.022–0.032 ms across the two runs. At 41 m, it reduced that median by 0.014–0.019 ms. Both voxel builds reduced triangle work substantially in the native viewport at 41 m and 100 m. At 41 m the voxel builds still took more time in the uncached Shadow Depths scope than their paired triangle-only controls. The static tests measure one oak with frozen geometry, and the timings leave forest-scale streaming and VSM caching open.

## Hierarchy comparison

The custom renderer selected coarse spray triangles in both camera and shadow views with frustum culling enabled. It needed 1,036 camera group tests and 8,625 per-part tests at 18 m, then 14 group tests and zero per-part tests at 41 m. All 4,730 spray placements still contributed at 41 m. The tested bough proxy covered 371 sprays at 100 m, reduced camera triangles by 89,210, and made no shadow substitution at the measured positions. Our budget is 1 px of projected sampled spray error for the camera and 0.75 texels in the light view.

In the native viewport at 41 m, the original voxel build reported zero separately visible assembly parts and the optimized voxel build reported 1,259. By 100 m, all four builds reported zero separately visible assembly parts while the tree still drew. Nanite's parent hierarchy has merged the part geometry there. The triangle-only results show how much merging is possible before adding voxels. This suggests measuring reusable parent cuts across several boughs as a separate step from evaluating voxel coverage. [Epic's Nanite Assemblies documentation](https://dev.epicgames.com/documentation/unreal-engine/nanite-assemblies?application_version=5.8)
