# Batched tree submission at 4K

```powershell
& 'C:\Users\samee\GitProjects\CustomGameEngine\build\Release\CustomGameEngine-TreeBatch.exe' --review-capture --scene forked-oak --tree-count 16 --camera overview --distance 41 --tree-submit batched
```

Use `--tree-count 1` for a single oak or `--tree-count 64` for the larger stand. Press **F1 → Spray LOD → Tree submission** to switch between **CPU direct**, **GPU selection + indirect**, and **GPU batched + indirect**. **Color by → LOD level** shows the selected cuts. The ordinary viewer opens with CPU direct.

## Renderer

The coarse traversal uses the existing tree hierarchy and projected error tests. It emits a selected parent or a range of spray placements. A second GPU dispatch processes the ranges in parallel across the view. The renderer counts and packs instances by draw bin, then emits indexed indirect draws for compatible geometry and materials. Tree and spray transforms use separate instance streams so a draw can cover multiple trees. The graph runs for the camera and for each active shadow view. [Renderer](../src/TreeGpu.cpp) · [Batched compute stages](../shaders/TreeBatch.hlsl) · [Shared selection math](../shaders/TreeCull.hlsli)

The staging buffer and compacted stream reserve for the maximum number of instances the baked cuts can emit. That reservation is about 218 MiB for the 64-oak scene. The measured tests use indexed triangle draws with depth culling off in all three modes. The per-part GPU control exposes its optional twig depth test.

## Replay

The main comparison uses the previous test's shared LOD 4 inputs at 3840 × 2160, 1× MSAA, 1 px camera error, 0.75 texel shadow error, fixed lighting, and frozen wind. The camera distances are measured from the stand's center oak. Each stand uses the same oak mesh with deterministic positions, yaw, and scale. All modes use the same stand for each comparison. The RTX 5090 ran two 61-frame samples per pose with reversed mode and pose order on the repeat. The normal Release assets were also tested at all five poses. [Timed captures](../build/oak-tree-batch/timings.csv) · [4K replay commands](../build/oak-tree-batch/capture.ps1)

### 41 m medians with shared inputs

| Trees | Mode | CPU recording | Camera GPU | Shadow GPU | Frame GPU | Camera / shadow submissions |
| ---: | --- | ---: | ---: | ---: | ---: | ---: |
| 1 | CPU direct | 0.532 ms | 0.767 ms | 0.229 ms | 0.998 ms | 76 / 33 |
| 1 | GPU per part | 0.034 ms | 0.913 ms | 0.252 ms | 1.168 ms | 6 / 6 |
| 1 | GPU batched | 0.033 ms | 0.806 ms | 0.246 ms | 1.054 ms | 6 / 6 |
| 16 | CPU direct | 4.920 ms | 8.123 ms | 3.613 ms | 11.737 ms | 639 / 528 |
| 16 | GPU per part | 0.301 ms | 9.248 ms | 3.998 ms | 13.247 ms | 96 / 96 |
| 16 | GPU batched | 0.098 ms | 8.174 ms | 3.633 ms | 11.812 ms | 6 / 6 |
| 64 | CPU direct | 5.650 ms | 14.673 ms | 14.411 ms | 29.085 ms | 1,344 / 2,112 |
| 64 | GPU per part | 0.957 ms | 16.939 ms | 15.911 ms | 32.857 ms | 384 / 384 |
| 64 | GPU batched | 0.112 ms | 14.788 ms | 14.460 ms | 29.233 ms | 6 / 6 |

At 64 trees, camera draw packets fell from 1,344 to 250 and shadow draw packets fell from 2,112 to 33. Per-pass submissions fell to six. The batched camera spent about 0.008 ms traversing, 0.050 ms selecting, and 0.048 ms packing. The tree draws accounted for 14.628 ms. CPU recording is measured before the GPU fence.

### GPU frame time across the five poses

Each cell shows CPU direct / GPU per part / GPU batched, in milliseconds. The values use the shared-input repeat.

| Distance | 1 tree | 16 trees | 64 trees |
| ---: | ---: | ---: | ---: |
| 14 m | 1.693 / 1.863 / 1.740 | 9.318 / 10.481 / 9.414 | 26.195 / 29.721 / 26.389 |
| 18 m | 1.600 / 1.741 / 1.595 | 9.075 / 10.266 / 9.143 | 25.949 / 29.406 / 26.064 |
| 24 m | 1.454 / 1.582 / 1.451 | 8.797 / 10.044 / 8.732 | 25.553 / 29.073 / 25.786 |
| 41 m | 0.998 / 1.168 / 1.054 | 11.737 / 13.247 / 11.812 | 29.085 / 32.857 / 29.233 |
| 100 m | 0.671 / 0.726 / 0.711 | 9.961 / 11.013 / 9.977 | 37.672 / 41.968 / 37.563 |

## Visual checks

The two GPU modes produced identical geometry masks and camera and shadow triangle totals across the static replay and 186 moving frames. The motion runs cover the 54-frame orbit for each stand size and 24 additional frames with moving wind. CPU direct produced identical masks as well. Its triangle totals differed at two static stand poses and 16 moving frames, with a largest difference of 2,088 triangles in a roughly 99M-triangle camera cut. The two GPU modes agreed in every one of those cases.

The regular shaded stills differed by up to 400 pixels when comparing the batched path with CPU direct. The whole-spray control with frustum culling off and the 4× MSAA check also preserved the GPU geometry masks. In group coloring, the batched path shades the original trunk the same way CPU direct does. The per-part debug view colors that trunk gray when it falls back to source geometry. [Static comparisons](../build/oak-tree-batch/parity.csv) · [Motion comparisons](../build/oak-tree-batch/motion.csv)

The 16-tree Release window opened with a 3840 × 2160 render target and 1× MSAA. The default cube capture and both Release test suites passed. [Window check](../build/oak-tree-batch/verified-window-title.txt)
