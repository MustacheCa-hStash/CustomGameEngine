# Can the oak baker transfer to a remodeled crown?

**Yes for this asset family.** The engine’s meshoptimizer baker, hierarchy builder and batched GPU traversal processed the old single-shoot oak and the remodeled four-shoot oak from their source GLBs. We reused the bake recipes and generated geometry, bounds, errors and placement orders afresh. Neither run used decoded Nanite data, a selected bough ID or a distance lookup table. The two assets share leaf topology, UV conventions and materials. Cross-species transfer remains open.

## Geometry and performance

The viewer uses the upper-crown lighting, transmission, ground bounce and all 2,072 shoot placements. The following are medians of three interleaved 31-frame run medians per arm at 4K on an RTX 5090. Camera counts include three backdrop triangles. Camera tree time covers the shaded asset pass. The previous preview already had reduced sprays. [Run medians](../build/oak-crown-optimized/repeat/runs.csv)

| View | Previous preview → optimized camera triangles | Camera tree GPU time | Whole-frame GPU time |
|---|---:|---:|---:|
| One oak, 28 m | 26.83M → 9.69M | 3.108 → 1.147 ms | 4.559 → 1.312 ms |
| Four oaks, 41 m | 107.33M → 36.60M | 11.501 → 3.690 ms | 17.073 → 4.074 ms |
| Four oaks, 100 m | 107.33M → 33.19M | 10.856 → 2.986 ms | 16.434 → 3.376 ms |

These are end-to-end results for the two viewer paths. The optimized path also does hierarchical frustum culling, batched submission and independent shadow selection. A fresh rebake reproduced all twelve binary sidecars byte-for-byte and the 41 m mask pixel-for-pixel. Two shaded captures differed at 24 of 8,294,400 pixels.

The geometry reference was the **full-detail remodeled oak**, 479.80M triangles per tree with instances expanded. At 41 m, four optimized oaks retained 97.32% foreground intersection-over-union versus source, with 2.59% of outline pixels missing their counterpart by more than one pixel. The 3.2 m leaf view retained 99.26% overlap. The optimized and previous-preview 41 m shadow submissions were 3.88M and 107.33M triangles respectively. The mask scores measure camera geometry. [Still quality](../build/oak-crown-optimized/guarded/stills.csv) · [Close leaves](../build/oak-crown-optimized/review/edge.jpg) · [Grove](../build/oak-crown-optimized/review/grove.jpg)

The standalone GLB contains **8,518,605 triangles per expanded tree**, including the full 364,604-triangle scaffold. It occupies 28,687,556 bytes versus 85,101,856 for the authoring GLB. The export retains all ten embedded textures, materials, instance transforms and source vertex attributes used by its triangles. Its single canopy tier works for a mid-distance placement. At 3.2 m its one-pixel outline miss reached **18.98%**, versus **1.08%** for the adaptive viewer. Use the adaptive viewer for the underside shot. [Portable GLB](../assets/source/white-oak-crown-study/WhiteOak-OpenCrown-Simplified.glb) · [Portable mesh checks](../build/oak-crown-optimized/portable-qa/stills.csv)

## Findings to take back to the simplification work

1. **Geometry generation transferred.** The adapter discovers the active glTF scene, repeated mesh nodes, material roles and scaffold. It calls the existing `BakeFixedLod` on each primitive and `BakeSprayBough --hierarchy` on each leaf/wood pair. The new tree produced eight material bakes and four hierarchies. The former oak produced two material bakes and one hierarchy. Feeding the former oak’s fresh bakes through the legacy and manifest loaders produced identical 4K masks and exactly 18,861,389 camera triangles at 41 m using unrestricted 1 px traversal. [Adapter](../Tools/BakeInstancedSprays.py) · [Parity captures](../build/oak-crown-optimized/parity)
2. **Placement ownership needed work.** The four shoots have 725, 497, 290 and 560 placements. Each needs its own order, group tags, material bakes, selection table and counters. A private viewer overlay registers all four and keeps one runtime scaffold. The legacy per-pair format still serializes the scaffold four times on disk. The study viewer keeps full source and candidates resident. The standalone GLB size is a separate measurement from VRAM usage. [Viewer changes](../Tools/OakLightingStudy/spray_families.patch)
3. **A rejected simplification is a useful result.** The 950-triangle shared far twig candidate passed on the fan and sparse extension. It introduced a collapsed UV triangle on the drooping and compact shoots. Both retained the baker’s component-wise far candidate. The fallback responds to the same validation failure on any input shoot.
4. **Geometric pixel error alone gave the wrong tradeoff.** At 100 m and 197°, unrestricted 1 px retained 90.42% silhouette overlap. Reducing the global budget to 0.2 px improved that view to 96.94%, but at 41 m it still submitted 106.82M triangles. The selected policy uses 1 px and the tested source-index LOD 4 as the coarsest camera shoot mesh. Near shoots can select finer whole-mesh tiers. The camera traverses canopy groups for culling, skips coarser whole-canopy proxies, and continues to simplify the scaffold. Shadows retain independent grouped and local cuts. The camera floor still costs about 33.19M triangles across four trees at 175 m. [0.2 px timings](../build/oak-crown-optimized/qualified/timing.csv) · [Selected cuts](../build/oak-crown-optimized/guarded/timing.csv)

## Motion and limits

Four animated-wind paths covered 157 frames and 153 transitions at 4K, 1× sampling. The source and candidates used the same camera and wind phase at each frame. The slow path covered 6 m and rotated 5° over two seconds on a fresh heading. Other paths sampled larger pose changes. Temporal error measures disagreement between the source and candidate frame-to-frame mask changes, divided by the union of the four masks. Shaded motion was also captured for visual inspection. [Frame measurements](../build/oak-crown-optimized/guarded/motion.csv) · [Shaded motion](../build/oak-crown-optimized/review/motion.webp)

| Path | Frames | Worst overlap | Worst outline miss >1 px | p95 temporal error |
|---|---:|---:|---:|---:|
| 41–100 m, 31° orbit | 16 | 96.79% | 2.44% | 5.07% |
| 50–56 m, reverse 12° orbit | 8 | 97.07% | 2.50% | 4.72% |
| 100–175 m, 26° orbit | 12 | 96.78% | 2.11% | 4.99% |
| 50–56 m, 3 m/s | 121 | 96.88% | 2.45% | 5.31% |

On the former oak, the same guarded setting achieved 98.97% overlap at 41 m and at least 98.70% on a separate ten-frame 50–100 m orbit. The recipe still assumes one static scaffold and one opaque modeled leaf/wood pair per instanced shoot, UV-V leaf fitting and the former oak’s numeric candidate targets. The choice to trust tier 4 came from rendered comparisons. The engine does not yet discover that limit on an arbitrary asset. [Former-oak motion](../build/oak-crown-optimized/parity/former-motion.csv)

**Next general test.** Freeze the adapter and validation policy, process a dense modeled broadleaf with a different leaf layout, and test a cheap alpha-card tree for pass-through. Store measured camera eligibility per candidate so individual distant groups can earn cheaper cuts. Comparing with Nanite on this tree would require a matched Unreal bake and capture.

## Reproduce in this checkout

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\Tools\BuildOakUpperCrownLod.ps1 -View -Trees 4 -Distance 41
```

Use `-Pose edge -Trees 1` for the underside, `-Distance 100` for the farther view, `-Unrestricted` for the original candidate selection, or `-Rebake` to regenerate the sidecars. The default build uses the frozen lighting-study renderer and stages the viewer under `build/oak-crown-optimized`. To repeat the benchmark, use Python with NumPy and Pillow and a fresh output directory. [Benchmark](../Tools/OakLightingStudy/check_crown_lod.py)

```powershell
python -B Tools/OakLightingStudy/check_crown_lod.py --output build/oak-crown-optimized/recheck
```
