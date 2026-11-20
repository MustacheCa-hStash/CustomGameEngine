# Two-pass oak occlusion at 4K

```powershell
& 'C:\Users\samee\GitProjects\CustomGameEngine\build\Release\CustomGameEngine-TreeOcclusion.exe' --review-capture --scene forked-oak --tree-count 16 --camera overview --distance 24 --yaw-degrees 132 --pitch-degrees -5.46 --msaa 1 --spray-pixel-error 1 --wind-strength 0 --probe-gi off --tree-submit batched --tree-occlusion on
```

Open **F1 → Spray LOD** and toggle **Two-pass camera occlusion**. The inspector reports how many bounds were deferred, how many current depth confirmed were hidden, and how many drew in the recovery pass. **Color by → LOD level** shows the active LODs. Use `--tree-count 1` or `--tree-count 64` to change the stand size.

## Renderer

The batched GPU path evaluates selected leaf, twig, bough, and trunk ranges against the previous frame's depth pyramid. It can split a range into its existing contiguous child clusters when some of them test as hidden. The first pass draws the likely-visible ranges. The second pass rebuilds depth from that draw, retests the entire deferred queue, and draws the ranges that current depth cannot prove hidden. The frame then builds the pyramid for the next camera view. [Indirect submission](../src/TreeGpu.cpp) · [Cluster queries and retest](../shaders/TreeBatch.hlsl)

The pyramid stores the farthest depth over each tile and all MSAA samples, including uncovered samples. Cluster bounds include the full wind displacement for the frame being tested. A range that crosses the near plane or leaves the testable viewport draws conservatively. The first frame, scene changes, and MSAA changes start without occlusion history. Shadows select geometry from the light view. [Depth reduction](../shaders/TreeDepth.hlsl)

## Matched runs

The RTX 5090 replay uses 3840 × 2160, 1× MSAA, a 1 px camera error budget, frozen wind, and fixed lighting. The matched asset uses the previous comparison's shared LOD 4 inputs. Camera time includes both occlusion passes and both depth builds. Each result is the median of 61 frames. The matched inputs have a second run with reversed test order. The figures below come from that repeat. [All distances and timing stages](../build/oak-two-pass/comparison.csv) · [Capture script](../build/oak-two-pass/capture.ps1)

| Trees | Distance | Camera triangles off → on | Camera GPU off → on | Shadow GPU off → on |
| ---: | ---: | ---: | ---: | ---: |
| 1 | 14 m | 9,468,504 → 8,666,905 | 1.445 → 1.505 ms | 0.250 → 0.250 ms |
| 1 | 18 m | 8,755,481 → 7,989,089 | 1.337 → 1.349 ms | 0.249 → 0.249 ms |
| 1 | 24 m | 7,458,682 → 6,850,379 | 1.196 → 1.253 ms | 0.247 → 0.247 ms |
| 1 | 41 m | 4,067,945 → 3,853,829 | 0.807 → 0.868 ms | 0.246 → 0.246 ms |
| 1 | 100 m | 1,483,357 → 1,483,357 | 0.464 → 0.572 ms | 0.245 → 0.245 ms |
| 16 | 24 m | 32,268,295 → 29,529,387 | 5.208 → 4.828 ms | 3.625 → 3.625 ms |
| 16 | 41 m | 46,892,852 → 36,627,406 | 8.268 → 6.693 ms | 3.630 → 3.626 ms |
| 64 | 41 m | 76,174,238 → 57,405,695 | 14.954 → 11.469 ms | 14.427 → 14.429 ms |
| 64 | 100 m | 110,088,744 → 88,598,447 | 23.160 → 18.740 ms | 14.426 → 14.429 ms |

The 64-tree, 41 m camera submits 24.6% fewer triangles. Frame GPU time drops from 29.386 to 25.895 ms and CPU recording rises from 0.117 to 0.131 ms. The option starts off. The five matched single-oak views each add 0.012–0.108 ms to the camera pass.

The launch command uses the normal Release assets. At 16 trees and 24 m, camera triangles go from 41,090,094 to 36,690,549 and camera GPU time goes from 5.567 to 5.112 ms. The normal Release single oak at 14 m also benefits, from 5.202 to 4.936 ms. [24 m occlusion off](../build/oak-two-pass/release/timed/n16-q24-off.png) · [24 m occlusion on](../build/oak-two-pass/release/timed/n16-q24-on.png)

With camera cluster frustum culling enabled, adding occlusion at 64 trees and 41 m drops the normal Release camera from 101,281,404 to 73,325,574 triangles and from 21.176 to 15.439 ms. The main table isolates the occlusion change with cluster frustum culling off.

The single-oak triangle counts fall below the prior Unreal HZB counts at 14 and 18 m. At 24, 41, and 100 m, the custom renderer submits 1.12×, 1.59×, and 1.42× as many triangles. These counters measure geometry sent to rasterization. The two engines still choose different LOD cuts at the nominal 1 px setting. [Prior Unreal counts and coverage](OAK_MATCHED_TRIANGLE_COMPARISON.md)

## Validation

The static geometry masks and all 186 motion masks matched pixel for pixel. The shaded stills differed in up to 707 pixels. The replay includes 54 frames for each stand size and 24 frames of animated wind. Every deferred range was accounted for by the second pass. The peak recovery was 131,212 ranges in the 64-oak orbit and 74,932 ranges with animated wind. The shadow cuts matched throughout. The MSAA, whole-spray, and camera-frustum controls also matched. [Frame-by-frame results](../build/oak-two-pass/motion.csv) · [Render controls](../build/oak-two-pass/controls.csv)

Trunk ranges take part in the query. In the standard replay, the conservative bounds retained the trunk triangles and the geometry savings came from leaves and twigs. At 64 trees and 41 m, the renderer discarded 12.82 million leaf triangles and 5.95 million twig triangles.

Both Release fixture suites passed. The launch command opened a 3840 × 2160 render target in the desktop window. [Release test log](../build/oak-two-pass/ctest.log) · [Window check](../build/oak-two-pass/verified-window-title.txt)
