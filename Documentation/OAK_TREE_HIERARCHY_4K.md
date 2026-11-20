# Forked oak geometry hierarchy and draw replay

The asset build cooks a fingerprinted `.treelod` sidecar from the tree GLB and the existing spray bakes. Each node stores its bounds, a measured geometric error, a geometry index range, and the children and placement interval it covers. The viewer frustum-tests each node, accepts its geometry when it fits the projected error budget, and otherwise descends. Leaves and twig wood choose their cuts independently. Trunk geometry uses the same traversal, with the exported trunk as its finest cut.

```powershell
& 'C:\Users\samee\GitProjects\CustomGameEngine\build\Release\CustomGameEngine-Hierarchy.exe' --forked-oak
```

Press **F1** and open **Spray LOD**. **Geometry hierarchy** is the automatic default. **Single bough proxy** selects the benchmark control. **Color by → LOD level** shows parent geometry in amber. **GPU indirect submission** switches the consumer of the selected draw list. Press **M** if the title shows 4× MSAA and you want the single-sample benchmark setting.

The canopy bake partitions spray placements spatially and generates parent geometry at two scales. The small regions contain 73–74 sprays and the large regions contain 295–296. The baker fits area-preserving leaf triangles and evaluates simplified twig meshes against the assembled LOD 4 wood. Trunk reduction updates vertex attributes and restores source faces that exceed the sampled transition allowance. The baker sorts trunk candidates by triangle count and discards dominated cuts. Identical vertices are shared across cuts. The finished sidecar is about 148 MiB and the viewer uploads its geometry into GPU-local buffers. [Baker](../Tools/BakeSprayBough.cpp) · [Asset format](../src/TreeHierarchy.h)

## Comparison method

The runs use 3840×2160, 1× MSAA, frozen wind, the same combined lighting, a 1 px camera budget, and the existing 0.75-texel shadow budget. Each measured case has two runs of 61 frames. The second run reverses the mode order at each distance. Timing runs use the RTX 5090 and GPU timestamps.

The shared-input comparison starts from the same LOD 4 spray meshes and camera poses as the Unreal benchmark. It applies the measured LOD 4-relative errors to the hierarchy. The normal Release view uses source-relative errors and finer near geometry. The Nanite table compares tree triangles at the same nominal 1 px setting with frustum culling enabled and depth occlusion disabled. The per-engine fixed-input geometry mask is the coverage reference.

## Triangle counts

| Distance | Previous cut | Hierarchy | Nanite | Previous gap closed |
| ---: | ---: | ---: | ---: | ---: |
| 14 m | 11,646,837 | 11,646,837 | 11,358,109 | 0.0% |
| 18 m | 11,312,912 | 11,312,912 | 10,418,802 | 0.0% |
| 24 m | 10,020,142 | 9,769,597 | 6,516,633 | 7.2% |
| 41 m | 5,902,564 | 5,078,504 | 2,513,534 | 24.3% |
| 100 m | 3,129,561 | 1,483,357 | 1,073,068 | 80.0% |

The trunk saves 250,545 triangles at 24 m. At 41 m the trunk saves 517,222 and baked twig parents replace the individual wood for 2,661 sprays, saving 306,838 more. By 100 m the parent cuts cover all 4,730 sprays. Twig triangles fall from 1,906,075 to 863,617 and leaf triangles fall from 417,909 to 331,385. The trunk and main boughs fall from 805,577 to 288,355. The shadow pass selects its own cut and submits 1,478,342 triangles at each view, down from 3,129,561.

The 100 m geometry masks overlap the fixed input by 94.45% for the new hierarchy and 95.89% for Nanite at 1 px. Over the 54-frame retreat, the lowest overlap with the previous cut was 97.17% at 114 m. Occupied tree pixels changed by at most 0.57%. [Comparison data](../build/oak-tree-hierarchy/comparison.csv) · [Moving captures](../build/oak-tree-hierarchy/hierarchy-sweep.mp4)

The coverage probe adjusts the camera budget in the shared-input viewer. The rows below are the closest sampled mask overlaps with Nanite's 1 px cuts. At 100 m this recovers most of the difference in coverage and leaves a 1.56M triangle gap. The cut at that pose uses 891K leaf triangles, 1.45M twig triangles, and 288K trunk triangles. [Coverage sweep](../build/oak-tree-hierarchy/coverage.csv)

| Distance | Custom budget | Custom triangles | Nanite triangles | Custom mask overlap | Nanite mask overlap |
| ---: | ---: | ---: | ---: | ---: | ---: |
| 24 m | 1.19 px | 8,842,134 | 6,516,633 | 98.94% | 98.93% |
| 41 m | 1.138 px | 3,925,192 | 2,513,534 | 97.00% | 97.02% |
| 100 m | 0.50 px | 2,630,054 | 1,073,068 | 95.79% | 95.89% |

## Same-cut replay

The CPU selects and records the same tree draw list in both modes. Direct mode records the indexed draws individually. Indirect mode places those records in an argument buffer and uses D3D12 `ExecuteIndirect` for each compatible batch. The GPU handles drawing and per-record bindings. The recorded-cut hashes and count columns cover the camera and the shadow pass. [D3D12 indirect drawing](https://learn.microsoft.com/en-us/windows/win32/direct3d12/indirect-drawing)

The direct and indirect draws produced identical shaded pixels and geometry masks across the measured poses. The 54-frame orbit and retreat also matched in geometry masks and in camera and shadow cut hashes. [All timings](../build/oak-tree-hierarchy/timings.csv) · [Switches in motion](../build/oak-tree-hierarchy/motion.csv)

| Shared-input view | Direct → indirect camera submissions | Direct → indirect shadow submissions | Hierarchy camera GPU, direct → indirect |
| ---: | ---: | ---: | ---: |
| 14 m | 26 → 3 | 33 → 3 | 1.733 → 1.749 ms |
| 18 m | 30 → 3 | 33 → 3 | 1.634 → 1.637 ms |
| 24 m | 34 → 3 | 33 → 3 | 1.450 → 1.450 ms |
| 41 m | 56 → 4 | 33 → 3 | 0.875 → 0.874 ms |
| 100 m | 36 → 3 | 33 → 3 | 0.443 → 0.444 ms |

These are medians from the second run. The 41 m camera submission time was about 2.8 µs direct and 1.8 µs indirect. Whole-frame CPU time in that case was 0.486 and 0.490 ms, respectively. At 100 m, the previous shared-input camera pass took 0.396 ms and the hierarchy took 0.443 ms. The normal Release case went from 0.376 to 0.444 ms. The triangle reduction and the submission batching each succeeded in their measured scope. The single-tree camera pass still costs more at 100 m.

The Release executable launched with a 3840×2160 renderer target. Both existing Release test suites passed.

## Remaining work

- The largest matched-input triangle gaps are at 24 and 41 m. The tree still uses the individual leaf cuts there. Improve small leaf and twig candidates, evaluate error on the assembled cut, and tighten the projected bounds used inside each spray.
- The far hierarchy builds leaf, twig, and trunk geometry as separate parts. Coarser cuts that can reduce them together and retain coverage across angles are the next geometry opportunity.
- The replay puts GPU indirect execution in place. The CPU still traverses, tests visibility, and builds the packets. GPU traversal, GPU packet compaction, and hierarchical depth occlusion remain separate comparisons. A stand of trees is the next useful test for CPU scaling.
- The hierarchy resides in memory at scene load. Compressed cluster pages, streaming, and the raster path for very small triangles remain architectural differences from Nanite. [Nanite geometry overview](https://dev.epicgames.com/documentation/unreal-engine/nanite-virtualized-geometry-in-unreal-engine?application_version=5.8)
