# Forked white oak geometry and GPU selection at 4K

```powershell
& 'C:\Users\samee\GitProjects\CustomGameEngine\build\Release\CustomGameEngine-GpuTree.exe' --forked-oak
```

Press **F1** and open **Spray LOD**. **CPU direct** uses the new geometry. To see the compute path, set **Tree submission** to **GPU selection + indirect**. Enable **Depth culling for twig wood** to test occlusion. **Color by → LOD level** highlights the active cuts, with parent geometry in amber. The title bar reports the 3840 × 2160 render target.

## Geometry bake

The existing fixed LOD baker tries more intermediate leaf cuts per group. It measures their edges and surfaces against the source and shared LOD 4 spray. It also rebuilds and measures complete twig sprays so that the selected wood can cover nearby faces across the original twig groups. It keeps twig cuts that improve triangle count or measured error. The renderer feeds these choices into the same hierarchy on the CPU and GPU. [Baker](../Tools/BakeFixedLod.cpp) · [Sidecar format](../src/FixedLod.cpp)

The replay uses the same 4,730 placements, 4K cameras, combined lighting, frozen wind, and 1× MSAA. It holds the camera budget at 1 px and the shadow budget at 0.75 texel. The shared-input tests use the LOD 4 leaf and twig meshes imported into Unreal and measure coverage against each renderer's own fixed reference. The table compares tree triangles queued for rasterization with frustum culling on and depth occlusion off. Unreal's figures are the earlier 4K offscreen Nanite captures. [Matched-input setup](OAK_MATCHED_TRIANGLE_COMPARISON.md) · [Raw comparison](../build/oak-gpu-selection/comparison.csv)

| Distance | Previous hierarchy | New hierarchy | Nanite | New mask overlap | Nanite mask overlap |
| ---: | ---: | ---: | ---: | ---: | ---: |
| 14 m | 11,646,837 | 9,468,504 | 11,358,109 | 99.807% | 99.999% |
| 18 m | 11,312,912 | 8,755,481 | 10,418,802 | 99.357% | 99.910% |
| 24 m | 9,769,597 | 7,458,682 | 6,516,633 | 98.728% | 98.929% |
| 41 m | 5,078,504 | 4,067,945 | 2,513,534 | 97.110% | 97.021% |
| 100 m | 1,483,357 | 1,483,357 | 1,073,068 | 94.447% | 95.889% |

The closest sampled mask overlaps give a separate comparison of geometry cost. The lower 14 and 18 m counts in the nominal 1 px run come with a larger loss of coverage. At similar overlap the 24 m cut saves about 947,000 triangles and the 41 m cut saves about 90,000. At 100 m the cost falls about 108,000 triangles at a slightly higher overlap than in the previous replay. Twig wood still accounts for 1.45M of the 2.52M triangles there. [Coverage sweep](../build/oak-gpu-selection/coverage.csv)

| Distance | Camera budget | Previous triangles at similar overlap | New triangles | Nanite | New mask overlap |
| ---: | ---: | ---: | ---: | ---: | ---: |
| 14 m | 0.46 px | 11,646,837 | 11,745,941 | 11,358,109 | 99.9992% |
| 18 m | 0.55 px | 11,312,912 | 10,627,462 | 10,418,802 | 99.912% |
| 24 m | 0.89 px | 8,842,134 | 7,894,951 | 6,516,633 | 98.923% |
| 41 m | 1.036 px | 3,925,192 | 3,835,160 | 2,513,534 | 97.026% |
| 100 m | 0.489 px | 2,630,054 | 2,522,518 | 1,073,068 | 95.890% |

In the normal Release scene, the tree uses 8.70M triangles at 24 m and 4.18M at 41 m, down 22.5% and 25.5%. The direct camera GPU time dropped from 1.600 to 1.348 ms at 24 m and from 0.807 to 0.659 ms at 41 m. CPU direct recording at 41 m increased from 0.252 to 0.537 ms as finer choices reduced the opportunity to select one cut for a group of placements. The lowest mask overlap with the previous bake over the 54-frame orbit and retreat was 98.06% at 48 m. [Shaded comparison](../build/oak-gpu-selection/24m-41m-100m.png) · [Moving captures](../build/oak-gpu-selection/release/motion/gpu-shaded) · [LOD captures](../build/oak-gpu-selection/release/motion/gpu-lod)

## Renderer

A compute workgroup per visible tree part and view traverses the hierarchy, stops when a visible parent fits the error budget, and compacts remaining placements by selected geometry. It fills indexed draw commands and count buffers consumed by the graphics pass. The CPU still dispatches per visible tree part. It reads counters after the frame fence for measurement. The depth option builds a conservative pyramid from the same frame after the leaves and trunk draw and applies it to the camera's twig nodes and fallback placements. [GPU traversal](../shaders/TreeSelect.hlsl) · [Depth pyramid](../shaders/TreeDepth.hlsl) · [D3D12 integration](../src/TreeGpu.cpp)

Both cut paths submitted the same camera and shadow triangle totals in the five static poses and all 54 moving frames. The geometry masks matched pixel for pixel in both modes, with and without twig depth culling. Front, side, and under-canopy masks also matched in the Release scene. A 4× MSAA mask check passed at 41 m. The GPU selection varied individual shaded pixels in the static checks, with at most 99 changed pixels in either asset setup with depth culling off. With depth culling on, the largest difference was 136 pixels. Overlapping surfaces can resolve in a different draw order. [Motion comparisons](../build/oak-gpu-selection/motion.csv) · [Shaded differences](../build/oak-gpu-selection/shaded-differences.csv)

The table uses the median of 61 frames from the reversed-order repeat on the RTX 5090. The CSV includes both runs, the Release results, and per-pass GPU timestamps. CPU time measures frame recording before the GPU fence. [Timings](../build/oak-gpu-selection/timings.csv)

| Shared-input distance | CPU recording, direct → GPU | Camera GPU, direct → GPU | Shadow GPU, direct → GPU | Depth rejects | Camera GPU cost of depth |
| ---: | ---: | ---: | ---: | ---: | ---: |
| 14 m | 0.644 → 0.040 ms | 1.456 → 1.530 ms | 0.229 → 0.259 ms | 29 | +0.120 ms |
| 18 m | 0.594 → 0.038 ms | 1.348 → 1.477 ms | 0.229 → 0.258 ms | 16 | +0.078 ms |
| 24 m | 0.552 → 0.034 ms | 1.217 → 1.330 ms | 0.229 → 0.257 ms | 5 | +0.080 ms |
| 41 m | 0.531 → 0.032 ms | 0.776 → 0.924 ms | 0.229 → 0.256 ms | 0 | +0.075 ms |
| 100 m | 0.024 → 0.032 ms | 0.442 → 0.472 ms | 0.228 → 0.251 ms | 0 | +0.068 ms |

At 41 m camera submissions fell from 76 to six. The depth pyramid removed 14,094 camera triangles at 14 m and 2,095 at 24 m in the shared-input run. The resulting GPU pass cost more at each sampled distance. The Release viewer defaults to CPU direct with depth culling disabled.

Nanite's cluster-scale cuts and occlusion remain the largest differences. At comparable coverage, the largest remaining geometry totals are the leaf cuts at 41 m and twig wood at 100 m. The next renderer scaling point is to distribute traversal and placement compaction across many GPU workgroups and measure a stand of trees. The current occlusion test consumes one tree's already drawn camera depth and targets twig bounds. Nanite tests smaller geometry clusters throughout its raster path. [Nanite technical details](https://dev.epicgames.com/documentation/unreal-engine/nanite-technical-details?application_version=5.8)

The Release window reported 3840 × 2160 with 1× MSAA. Both Release test suites passed. [Window check](../build/oak-gpu-selection/verified-window-title.txt)
