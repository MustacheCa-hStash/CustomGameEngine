# Oak cluster frustum culling at 4K

```powershell
& 'C:\Users\samee\GitProjects\CustomGameEngine\build\Release\CustomGameEngine-TreeClusters.exe' --review-capture --scene forked-oak --tree-count 16 --camera overview --distance 24 --tree-submit batched --tree-clusters on
```

Press **F1 → Spray LOD → Camera cluster frustum culling** to compare the modes from the same camera. The option works with **GPU batched + indirect** and the camera frustum test enabled. The inspector shows the number of bounds tested, the number rejected, and GPU time spent in cluster selection. **Color by → LOD level** shows the selected geometry.

## Renderer

The batched traversal first selects the camera cut at the existing error budget. Regular spray LODs use the baked meshlet ranges. Local spray cuts, parent meshes, and source geometry use the same meshlet scan when their GPU tables are prepared. The scan partitions the original triangle order so the cluster draws can use the same index buffers. [Meshlet packing](../src/MeshletPacking.h) · [GPU table preparation](../src/TreeGpu.cpp)

The renderer groups neighboring meshlets into a hierarchy of contiguous ranges. It queues the selected cut when its bounds cross the frustum. It rejects fully outside ranges, draws fully inside ranges as one packet, and descends through intersecting ranges. A cut whose extra fragments would exceed the scratch capacity uses its original draw. The bounds account for the full wind displacement. Shadow passes use the batched whole-cut path. [Cluster selection shader](../shaders/TreeBatch.hlsl) · [Frustum bounds tests](../shaders/TreeCull.hlsli)

## Replay

The tests cover 1, 16, and 64 oaks at 14, 18, 24, 41, and 100 m with a 3840 × 2160 target, 1× MSAA, fixed lighting, and a 1 px camera budget. Shared LOD 4 inputs use two 61-frame runs with reversed mode and pose order. The normal Release scene uses one 61-frame run. An 8 m view tests stronger clipping. Both modes use the same batched renderer with the cluster option toggled. [Capture commands](../build/oak-cluster-cull/capture.ps1)

| Shared-input view | Camera triangles off → on | Saved | Camera GPU off → on | Cut packets off → on |
| --- | ---: | ---: | ---: | ---: |
| 1 oak, 8 m | 8,683,194 → 7,585,491 | 12.6% | 1.440 → 4.954 ms | 27 → 2,040 |
| 1 oak, 14 m | 9,468,504 → 9,258,450 | 2.2% | 1.451 → 2.574 ms | 35 → 767 |
| 16 oaks, 24 m | 32,268,295 → 27,559,130 | 14.6% | 5.180 → 5.888 ms | 155 → 841 |
| 64 oaks, 24 m | 59,018,581 → 53,936,943 | 8.6% | 11.329 → 12.658 ms | 251 → 3,839 |
| 64 oaks, 41 m | 76,174,238 → 72,965,091 | 4.2% | 14.778 → 17.964 ms | 250 → 11,146 |
| 64 oaks, 100 m | 110,088,744 → 105,706,878 | 4.0% | 23.030 → 24.480 ms | 240 → 11,847 |

The table uses the reversed-order repeat at the five standard distances. The 8 m view uses its separate run. At 41 m with 64 trees the cluster stage took 3.469 ms. Six indirect submissions still carried the camera work, with many more packets inside each submission. The option is off by default because the cluster work and packet expansion cost more than the reduced geometry recovers in the matched views where it makes a material cut. [All distances and controls](../build/oak-cluster-cull/comparison.csv) · [GPU timings and counters](../build/oak-cluster-cull/timings.csv)

The launch command opens the normal Release asset. At that 16-tree, 24 m view, the count goes from 41,090,094 to 34,855,032 camera triangles and GPU camera time goes from 5.569 to 6.212 ms. A single oak at 41 or 100 m fits inside the camera and bypasses the cluster dispatch.

## Validation

The camera masks matched pixel for pixel across the static views and 186 moving frames, including the wind replay. The parent selection counters and shadow triangle counts matched. The largest frame-level reduction during motion was 11,733,953 camera triangles. The shaded stills differed in at most 591 pixels per frame. The 4× MSAA, whole-spray, and frustum-disabled controls passed. The arena kept every tested split within its capacity. [Motion results](../build/oak-cluster-cull/motion.csv)

The leaf and twig bakes reproduced their original bytes with the shared meshlet packer. Both Release test suites passed, and the launch command produced a 3840 × 2160 render target. [Release validation](../build/oak-cluster-cull/validation.log)
