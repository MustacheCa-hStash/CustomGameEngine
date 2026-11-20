# White oak leaf spray at 24 m and 41 m

The new leaf bake saves **525,837 camera triangles at 41 m** and **69,943 at 24 m** at comparable silhouette quality. Leaves account for 518,573 and 67,866 of those savings. The camera GPU gains are smaller than the triangle gains. The current Release build uses the new candidates with the existing spray-wide projection by default.

## Matched comparison

One forked white oak on the RTX 5090, 3840 × 2160, 45° vertical FOV, 1× sampling, frozen wind, warmed triangle-only cuts, frustum culling on and HZB off. Unreal uses the Preserve Area assembly and its 1 px setting. Both engines start from the same LOD 4 inputs and 4,730 saved placements. Each custom row uses the cheapest tested cut satisfying both Nanite quality measurements at that pose. The tests use the same quarter view and separate each engine’s fixed reference. The finer budget sweep also updates the previous renderer’s baseline. [Original calibration](OAK_MATCHED_TRIANGLE_COMPARISON.md) · [Outline measurement](../Tools/CompareOakMaskEdges.py) · [Aligned 4K crop comparison](../build/oak-leaf-step/matched-mask-crops.png)

| Distance and cut | Budget | Tree triangles | Leaf triangles | Mask IoU | Edge samples beyond 1 px | Camera GPU, ms |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| 24 m previous | 0.8828 | 7,920,834 | 5,080,789 | 98.934% | 2.141% | 1.249–1.254 |
| 24 m per-group bounds only | 0.8750 | 7,914,901 | 5,071,128 | 98.930% | 2.152% | 1.226–1.253 |
| **24 m new bake** | **0.8882** | **7,850,891** | **5,012,923** | **98.930%** | **2.161%** | **1.222–1.237** |
| 24 m Nanite | 1.0000 | 6,516,633 | — | 98.929% | 2.220% | 0.748–0.792 |
| 41 m previous | 0.9758 | 4,221,940 | 2,530,052 | 97.180% | 4.852% | 0.773–0.778 |
| 41 m per-group bounds only | 0.9688 | 4,234,342 | 2,533,984 | 97.184% | 4.845% | 0.765–0.770 |
| **41 m new bake** | **1.0187** | **3,696,103** | **2,011,479** | **97.148%** | **4.831%** | **0.764–0.767** |
| 41 m Nanite | 1.0000 | 2,513,534 | — | 97.021% | 4.856% | 0.563–0.609 |

The GPU ranges span two run medians with reversed order. Custom times cover the shaded camera pass using CPU direct submission, 61 timed frames per run. Unreal times are fresh offscreen measurements of the per-frame sum of Nanite visibility and base pass, 53–54 retained frames per run. The Unreal scopes exclude other lighting/compositing, and the custom pass uses its own lighting, sky and ground. The count comparison isolates tree geometry. The timings describe these workloads and leave whole-renderer speed parity open. IoU measures occupancy including canopy holes. The edge column counts boundary pixels farther than one pixel from a boundary in the other mask, in both directions. [Interleaved custom runs](../build/oak-leaf-step/final-timings.csv) · [Unreal profile jobs](../build/unreal-nanite-oak/leafstep-jobs.json) · [Raw Unreal GPU profiles](../build/unreal-nanite-oak/Saved/Profiling/CSV)

Holding each **previous** budget fixed isolates the leaf change. At 24 m, 50,850 leaf triangles disappear with exactly the same twig and trunk cuts. At 41 m, 228,003 disappear. Both fixed-budget comparisons improve IoU and one-pixel edge agreement. Retuning the camera budget to the Nanite targets accounts for the remaining savings in the table. At the nominal shared-input 1 px, the new totals are 7,416,028 and 3,821,801. [Budget sweep and part counters](../build/oak-leaf-step/outline-replay.log)

The final Release executable reproduced both selected triangle counts. Its fresh 61-frame camera medians were 1.233 ms and 0.762 ms. GPU-batched submission produced pixel-identical masks and identical shadow triangle counts at both poses. Its 41 m shared-input camera cut differed from direct submission by 14 triangles. Unreal triangle counts and reference masks come from the prior matched captures. Its GPU profiles were rerun for this assessment. [Matched Unreal counts](../build/oak-triangles-matched/unreal-counts.csv) · [Final build checks](../build/oak-leaf-step/shipping-views.log)

## What changed

1. **Projection experiment.** The CPU selector builds an AABB for each baked leaf group over its candidate vertices and measured displacement. It projects these bounds separately and uses the cheaper complete set of eligible cuts. On the old geometry at a fixed 1 px it saved 50,340 triangles at 24 m and 41,466 at 41 m. At matched quality the effect shrank to −0.07% and +0.29% of total triangles. CPU recording rose from roughly 0.52–0.60 ms to 1.50–1.63 ms. The inspector retains this experiment under **Spray LOD → Project individual leaf group bounds (CPU)**. The normal selector and the GPU modes use spray-wide projection. [CPU selector](../src/D3D12Renderer.cpp) · [Existing-cut control](../build/oak-leaf-step/control-replay.log)
2. **Bake experiment.** Each small LOD 4 leaf supplies additional edge-collapse candidates. The search retains its endpoint anchors and checks mesh connectivity, geometric winding and UV orientation. One search scores sampled rim and surface displacement. Another scores the leaf silhouette from seven orthographic directions. The baker compares candidate occupancy against both the authored leaf and the shared LOD 4 leaf. It retains existing cuts, tests cheaper replacements against the previous cut’s coverage loss, and adds fine candidates with small occupancy differences. The runtime reads the resulting cut table and its measured error. A geometry-only trial reached 7.72M triangles at 24 m but regressed to 5.47M at 41 m under the same quality gates. [Leaf cut generator](../Tools/BakeFixedLod.cpp) · [Offline silhouette sampler](../Tools/LeafOutline.h)
3. **GPU layout control.** Repacking existing leaf vertices in cut order produced the same triangles and increased camera time to 1.352–1.358 ms at 24 m and 0.868–0.869 ms at 41 m. The shipping path retains the existing vertex layout. The accepted bake increases the leaf sidecar from 8,002,976 to 8,133,008 bytes. The twig sidecar and the geometry payloads of the trunk and canopy parents reproduced the previous data.

## What carries over to the normal viewer

At the same **1 px source-relative setting** on the full authored tree, the 24 m quarter view falls from 8,697,120 to 8,668,824 triangles (−0.33%). The 41 m quarter view falls from 4,178,287 to 4,000,805 (−4.25%). All of those savings are leaves. The old and new camera medians span 1.335–1.339 and 1.316–1.335 ms at 24 m, and 0.638–0.655 and 0.645–0.656 ms at 41 m. The ordinary 41 m view has no demonstrated GPU-time gain. [Native Release timings](../build/oak-leaf-step/release-timings.csv)

Against the full 1.142-billion-triangle source, 24 m quarter-view IoU decreases by 0.003 percentage points and edge misses rise by 0.008 points. At 41 m IoU improves by 0.061 points and edge misses decrease by 0.074 points. The held-out 24 m front view has 0.019 points more edge misses. The held-out 41 m side view has 0.056 points fewer, or 0.058 points fewer with wind. On the shared LOD 4 fixture at the quarter-view-tuned budgets, that side view has 0.117 points more edge misses without wind and 0.092 more with wind. Only the quarter views were calibrated against Nanite. [Per-pose measurements](../build/oak-leaf-step/shipping-views.log)

In a separate 24-frame 4K capture, the camera retreats from 24 to 41 m while orbiting 32° with animated wind. The new native 1 px cut draws fewer triangles in every frame, saving 27,561–177,885 per frame. Mean IoU against the moving authored source improves by 0.034 points and mean edge misses decrease by 0.039 points. The largest increase in edge misses in any frame is 0.024 points. These measures compare each frame’s silhouette to its reference. Temporal shading, shadow fidelity and multi-tree frame time remain open. [Moving comparisons](../build/oak-leaf-step/release-motion.csv)

## Decision on step 2

**Proceed to a bounded twig-parent experiment.** The matched 41 m view still draws 1,396,269 twig triangles. At the previous 100 m quality-matched cut the twig parents draw 1,450,965, including 683,447 faces restored by the current repair loop. Try a connected-region refit on one small and one large parent, retaining branch junctions and exposed tips, and compare it with the existing parents at 41 m and 100 m. Record first visible depth, silhouette, caster quality, triangles and GPU time before expanding the change. [Prior breakdown and repair strategy](OAK_TRIANGLE_PRIORITIES.md)

The 24 m result leaves a separate leaf problem. Five million leaf triangles remain and the total is still 20.5% above Nanite. The next leaf experiment should simplify a small group of overlapping leaves together, where one leaf can cover a change to another, and score the full group’s openings across views. Another per-leaf bounds pass has little measured upside.

## Viewing the result

From the repository root, these commands build the standard Release viewer and open the normal source-relative oak at 41 m. Change the distance to 24 for the other pose. **F1 → Spray LOD → Color by → LOD level** shows the active cuts.

```powershell
$cmake = (Select-String '^CMAKE_COMMAND:INTERNAL=(.*)' build/CMakeCache.txt).Matches.Groups[1].Value
& $cmake --build build --config Release --target CustomGameEngine --parallel 6
.\build\Release\CustomGameEngine.exe --review-capture --scene forked-oak --distance 41 --yaw-degrees 132 --pitch-degrees -5.46 --msaa 1 --wind-strength 0 --lod auto --spray-mode hierarchy --spray-pixel-error 1 --tree-submit direct
```

To open the exact **shared-input** 41 m cut from the comparison, use the staged viewer below. At 24 m replace `41` with `24` and `1.0187` with `0.8882`. The native Release viewer measures errors from the full authored tree.

```powershell
.\build\oak-leaf-step\shipping-viewer\CustomGameEngine.exe --review-capture --scene forked-oak --distance 41 --yaw-degrees 132 --pitch-degrees -5.46 --msaa 1 --wind-strength 0 --lod auto --spray-mode hierarchy --spray-pixel-error 1.0187 --tree-submit direct
```
