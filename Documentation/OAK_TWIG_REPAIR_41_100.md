# White oak twig repair at 41 m and 100 m

**Proceed to step 3 on one bough.** A loose refit removed 6.1% and 12.8% of the small and large parent triangles. The whole tree still passed the Nanite silhouette gates, but the exposed twigs lost coverage at both distances and in the light view. A stricter refit reproduced the previous camera and light masks on both parents and saved only eight and ten triangles. None of the repairs produced a repeatable whole-tree GPU gain. The variants are available in a separate pilot build.

## Whole tree at matched silhouette quality

The baseline includes the [step 1 leaf bake](OAK_LEAF_SPRAY_24_41.md). Both engines use the same forked oak LOD 4 geometry and 4,730 placements, frozen wind, 3840 × 2160, 45° vertical FOV, 1× sampling, frustum culling and no HZB. Yaw is 132°, with pitch −5.46° at 41 m and −2.235° at 100 m. Each engine is scored against its own finest shared-input reference. For each distance we selected a common custom budget where every tested repair meets Nanite’s occupied-mask IoU and its fraction of silhouette edge pixels within one pixel. This includes gaps inside the crown. Recalibrating the baseline and keeping the budget fixed across repairs isolates the twig savings.

| Camera | Current tree | Loose repair | Intermediate | Anchored | Nanite |
| --- | ---: | ---: | ---: | ---: | ---: |
| 41 m triangles | 3,641,369 | 3,639,999 | 3,641,313 | 3,641,361 | 2,513,534 |
| 41 m GPU ms | 0.757 | 0.755 | 0.755 | 0.761 | 0.563–0.567 |
| 100 m triangles | 2,431,143 | 2,429,773 | 2,431,087 | 2,431,135 | 1,073,068 |
| 100 m GPU ms | 0.663 | 0.663 | 0.664 | 0.663 | 0.475–0.513 |

The natural camera cut takes the small parent at both distances. It descends through the large parent at both matched budgets. At **1.0261 px / 41 m**, current and anchored measure 97.125% IoU and 4.8552% of edge pixels beyond one pixel, versus Nanite’s 97.021% and 4.8557%. At **0.49034 px / 100 m**, current and anchored measure 95.892% and 6.154%, versus 95.889% and 6.702%. The camera saves eight triangles and the separately selected shadow cut saves ten in the anchored build. The gap to Nanite remains about 1.13M and 1.36M camera triangles. [All matched cuts and masks](../build/oak-twig-step/matched.csv) · [Nanite mask scores](../build/oak-gpu-selection/pixel-edge-audit.csv)

GPU measurements use the RTX 5090. Each custom value is the median of four 61-frame shaded-camera medians, alternating test order. At 41 m the current and loose run medians span 0.751–0.763 and 0.746–0.762 ms. At 100 m they span 0.662–0.665 and 0.661–0.664 ms. Unreal uses the Preserve Area triangle assembly and its 1 px setting. Its fresh measurements are two run medians of the per-frame sum of Nanite visibility and Nanite base pass (53–54 retained frames per run). Triangle counts use the matched tree-only frustum captures. Unreal and custom use different shaded pass scopes, so these times describe the measured camera workloads. [Shaded timing runs](../build/oak-twig-step/timed.csv) · [Nanite triangle counts](../build/oak-triangles-matched/unreal-counts.csv) · [UE 41 m profile](../build/unreal-nanite-oak/Saved/Profiling/CSV/twigstep-q41-r1.csv) · [UE 100 m profile](../build/unreal-nanite-oak/Saved/Profiling/CSV/twigstep-q100-matched-r1.csv)

## The two parents

| Twig parent | Sprays | Accepted grid faces | Restored faces | Previous total | Loose | Intermediate | Anchored |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| Small, node 56 | 74 | 11,919 | 10,715 | 22,634 | 21,264 | 22,578 | 22,626 |
| Large, node 164 | 296 | 28,244 | 25,663 | 53,907 | 46,983 | 53,899 | 53,897 |

The repair receives the faces the grid failed to cover. All three recipes keep the accepted grid triangles, use a normal- and UV-weighted topology simplifier on failed faces, lock open borders, check the fitted faces against the preceding twig surface, restore any remaining sampled holes, and measure the resulting parent against the assembled LOD 4 surface in both directions. The **loose** variant applies that process to the full failed index list. **Intermediate** constrains each connected patch’s longitudinal endpoints and limits change in projected triangle area to 5% on each world axis. **Anchored** protects all six axis extrema, uses one eighth the allowed simplifier displacement and a 0.5% area limit. The recorded sampled maximum LOD 4 errors remained 14.27 and 23.94 mm in all three recipes. The image tests distinguish their results. [Patch fitter](../Tools/TwigRepair.h) · [Baker integration](../Tools/BakeSprayBough.cpp) · [Loose bake log](../build/oak-twig-step/pilot.log) · [Intermediate bake log](../build/oak-twig-step/middle.log) · [Anchored bake log](../build/oak-twig-step/anchored-bake.log)

**Large parent in the camera.** We forced node 164 in both versions and descended through the other pilot node in both. This isolates the large parent while keeping the surrounding oak visible. These are diagnostic whole-tree cuts. Each row passes the same Nanite silhouette gates as the automatic cut.

| Camera | Current triangles / GPU ms | Loose triangles / GPU ms | Anchored triangles / GPU ms |
| --- | ---: | ---: | ---: |
| 41 m, 1.02156 px | 3,635,140 / 0.740 | 3,628,216 / 0.745 | 3,635,130 / 0.745 |
| 100 m, 0.48930 px | 2,404,052 / 0.653 | 2,397,128 / 0.652 | 2,404,042 / 0.653 |

**What the canopy hides.** An independent two-sided raster measures each parent alone against its LOD 4 reference in the two camera poses and in a 2048² orthographic view from the scene sun direction (24 m span). Depth and interpolated vertex normals use the nearest visible hit at common occupied pixels. The sun view measures caster geometry. Receiver shadows, wind transitions and forest scenes were outside this pilot. [Reproduce isolated measurements](../Tools/CompareTwigParents.py) · [Isolated twig crops](../build/oak-twig-step/twig-crops.png)

| Parent / camera | Previous → loose IoU | Previous → loose edge misses beyond 1 px | Previous → loose p95 absolute depth error |
| --- | ---: | ---: | ---: |
| Small, 41 m | 81.95% → 80.03% | 5.34% → 5.85% | 5.91 → 5.97 mm |
| Small, 100 m | 82.14% → 80.33% | 5.36% → 5.93% | 5.24 → 6.07 mm |
| Large, 41 m | 53.44% → 52.24% | 14.77% → 15.45% | 275 → 295 mm |
| Large, 100 m | 54.71% → 53.62% | 15.75% → 16.52% | 173 → 190 mm |

The loose light-view IoU fell from 81.24% to 79.17% on the small parent and 54.42% to 53.20% on the large parent. The anchored version changed zero mask pixels in all six isolated views, and p95 depth and normal error matched the controls at the reported precision. The intermediate fit was close, saving 56 and eight triangles, with a 0.066 percentage-point small-parent IoU loss at 41 m. [Loose local scores](../build/oak-twig-step/parents.csv) · [Intermediate scores](../build/oak-twig-step/parents-middle.csv) · [Anchored scores](../build/oak-twig-step/parents-anchored.csv) · [Changed pixels](../build/oak-twig-step/isolated-mask-differences.csv)

## Why move to step 3

The small parent’s 10,715 restored faces form 2,028 index-connected patches with a median size of three faces. The large parent has 4,491 patches with a median of four. Once the grid has discarded that connectivity, protecting patch edges and visible tips leaves few useful collapses. The existing large grid parent already has substantially worse isolated depth and coverage than its own LOD 4 reference. Spending more time on the last repair pass would leave that earlier loss in place.

On one bough, compare the current children and both parent tiers with a complete cut built from smaller branch-connected regions **before** the grid reduction. Give the selector at least one intermediate triangle budget and keep all fine children available. Test the parent alone and inside the crown, including its light-space depth. Use the same camera and timing controls. The two measured parents support that experiment; they do not establish a tree-wide savings rate.

## Build and inspect

Run from the repository root. The default pilot uses the anchored repair. Set `-DOAK_TWIG_REPAIR_MODE=loose` or `intermediate` on the configure line to inspect either of the other measured recipes. The standard Release executable remains the current-renderer control.

```powershell
$cmake = (Select-String '^CMAKE_COMMAND:INTERNAL=(.*)' build/CMakeCache.txt).Matches.Groups[1].Value
& $cmake -S . -B build -DOAK_TWIG_REPAIR_MODE=anchored
& $cmake --build build --config Release --target oak_twig_repair_pilot --parallel 6
.\build\Release\twig-repair\CustomGameEngine.exe --review-capture --scene forked-oak --distance 100 --yaw-degrees 132 --pitch-degrees -2.235 --msaa 1 --wind-strength 0 --lod auto --spray-mode hierarchy --tree-submit direct --spray-pixel-error 1
```

For 41 m use `--distance 41 --pitch-degrees -5.46`. The packaged viewer uses errors relative to the full authored tree. At its stock 1 px budget the 41 m camera draws 4,000,805 triangles with either build. At 100 m the stock count falls from 1,488,389 to 1,488,379. The anchored hierarchy reproduced the measured native pilot byte for byte. Both Release test suites passed. [Packaged viewer checks](../build/oak-twig-step/verified-viewer.log)

To inspect the exact shared-input automatic cut, use `.\build\oak-twig-step\anchored\CustomGameEngine.exe` with the same options and `--spray-pixel-error 1.0261` at 41 m or `0.49034` at 100 m. The staged `large-anchored` and `large-old` viewers reproduce the forced-large controls at the budgets listed above.
