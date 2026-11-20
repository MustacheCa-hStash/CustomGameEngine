# Forked white oak with matched triangle input

The earlier 4K table used the custom viewer's normal automatic cut and Unreal's optimized spray. The custom cut could draw finer leaf and twig levels close up. The Nanite triangle count came from the editor viewport, which selected a different cut than the offscreen camera. This run matches the source and reads the native count directly from the 4K offscreen camera.

## Counter and input

The custom counter multiplies the number of triangles in a selected indexed draw by the instance count. Nanite's `CalculateClusterStats` sums the triangle counts of clusters queued for hardware and software rasterization, including the second occlusion pass where active. Both counters include geometry that can later fail primitive or pixel tests. This run isolates the tree and compares both counters with frustum culling on and hierarchical depth-buffer occlusion (HZB) off. A second Unreal column shows the effect of enabling its HZB. [Custom draw counter](../src/D3D12Renderer.cpp) · [Epic's Nanite overview](https://dev.epicgames.com/documentation/unreal-engine/nanite-technical-details?application_version=5.8)

Both engines had the same trunk, 4,730 saved PVE transforms, a 1,504-triangle leaf mesh, and a 950-triangle twig mesh per spray. The staged custom viewer rebases its sampled transition errors on leaf and wood LOD 4 using the existing direct measurements. It keeps existing coarser geometry and starts the eligible cut at the same meshes that were imported for the Unreal Preserve Area assembly. The current bough recipe is also baked relative to that input. The test uses matching 3840 × 2160 camera poses, 45° vertical FOV, frozen wind, and no voxels. [Rebased transition data](../build/oak-triangles-matched/rebase.json)

For calibration, the fixed LOD 4 tree submits exactly **12,412,997** triangles with culling off in the custom engine. Unreal's finest tested cuts at 14 and 100 m counted **12,247,966** and **12,152,187** respectively at both 0.002 px and 0.000002 px. That leaves a 1.3–2.1% difference in the nominal full-geometry control. The two engines' reference masks overlap by **99.66–99.70%** across all five views. Overlap is the size of the intersection of the occupied-pixel masks divided by the size of their union. [Fixed-input Unreal checks](../build/oak-triangles-matched/ue-full-source.png) · [Mask measurements](../build/oak-triangles-matched/coverage.csv)

## 4K at the nominal 1 px setting

| Distance | Custom frustum only | Unreal frustum only | Unreal frustum and HZB | Custom / Unreal, frustum only |
| ---: | ---: | ---: | ---: | ---: |
| 14 m | 12,109,255 | 11,358,109 | 9,758,386 | 1.07× |
| 18 m | 12,412,997 | 10,418,802 | 9,516,983 | 1.19× |
| 24 m | 12,412,997 | 6,516,633 | 6,119,480 | 1.90× |
| 41 m | 9,915,557 | 2,513,534 | 2,416,483 | 3.94× |
| 100 m | 3,129,561 | 1,073,068 | 1,047,514 | 2.92× |

Disabling frustum culling as well leaves the 24 m, 41 m, and 100 m counts unchanged in both engines. Adding HZB saves 14.1% at 14 m, 8.7% at 18 m, 6.1% at 24 m, 3.9% at 41 m, and 2.4% at 100 m in Unreal. The largest mid-range differences here come from the selected LOD geometry. These counts measure geometry input to rasterization. Camera and shadow GPU effects require comparable pass and material settings. [Count matrix with all toggles](../build/oak-triangles-matched/comparison.csv) · [Raw offscreen counters](../build/oak-triangles-matched/ue-offscreen-counters.png)

## Coverage at comparable fidelity

Each simplified mask below is measured against its own engine's fixed reference mask at the same pose. These masks capture visible geometry coverage. Shaded detail, bark appearance, and motion remain additional acceptance checks.

| Distance | Cut | Triangles | Mask overlap with source |
| ---: | --- | ---: | ---: |
| 24 m | Custom at 1 px | 12,412,997 | 100.00% |
| 24 m | Unreal at 1 px | 6,516,633 | 98.93% |
| 24 m | Unreal at 0.50 px | 12,249,603 | 100.00% |
| 41 m | Custom at 1 px | 9,915,557 | 99.96% |
| 41 m | Unreal at 1 px | 2,513,534 | 97.02% |
| 41 m | Unreal at 0.42 px | 11,422,161 | 99.95% |
| 41 m | Custom, forced coarsest spray | 3,227,337 | 95.51% |
| 100 m | Custom at 1 px | 3,129,561 | 95.73% |
| 100 m | Unreal at 1 px | 1,073,068 | 95.89% |

The 41 m count gap at 1 px comes with a larger coverage change in the Nanite cut. At the closest sampled mask fidelity, the custom cut uses fewer triangles there. At 100 m Nanite retains a clear triangle advantage with closely matched coverage overlap. The measurements use one shared quarter angle at each distance. [41 m mask comparison](../build/oak-triangles-matched/matched-41m-coverage.png) · [Tolerance sweep results](../build/oak-triangles-matched/quality.csv)

## Next triangle pass

Bake intermediate leaf and twig cuts that can be selected per local group. Target the empty range between the current fine and coarse cuts at 24 and 41 m. The custom engine currently takes the maximum error across a spray before it selects a level. Its coarser fitted leaf levels also share the same maximum rim error in the direct comparison with leaf LOD 4. At 41 m that leaves 7.11M leaf triangles active even though some smaller groups could qualify for a coarser cut. Give each group its own measured transitions and retain the fine version where its silhouette or physical error warrants it.

Test leaf and twig candidates together. At 41 m, forcing the existing coarsest sprays yields 3.23M triangles with the full trunk and 95.51% mask overlap. The 1 px Nanite cut at that pose used 2.51M triangles with 97.02% overlap. Reaching that point therefore requires cheaper wood or parent geometry as well as a leaf improvement. At 100 m the comparable-coverage target requires more parent cuts across the crown and a trunk LOD. At 14–18 m retain the close-cut coverage and revisit cluster visibility if the gap remains.

Use all five 4K poses as acceptance points and hold the same masks, shaded angles, and motion views as references for each iteration. The target is the triangle count at comparable visible fidelity and the measured camera and shadow cost. That leaves the user’s 1 px geometric control available for runtime selection and gives us an independent visual test.
