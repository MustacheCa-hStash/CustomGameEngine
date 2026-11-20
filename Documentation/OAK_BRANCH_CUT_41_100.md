# White oak bough 7 before grid reduction

**Keep the connected cut experimental and spend another iteration on step 3.** This pilot replaces the spray wood on one authored bough, covering 371 of 4,730 placements. When each renderer must match Nanite’s exposed-bough silhouette, the branch cut saves 27.0% over the grid at 41 m and costs 0.8% more at 100 m. Judging only the whole crown hides the far failure. [Comparison data](../build/oak-branch-step-final/summary.csv) · [Exposed wood in both engines](../build/oak-branch-step-final/bough-quality-comparison.png)

## What the whole tree draws

The input is the forked oak with a shared 1,504-triangle leaf and 950-triangle wood spray, 805,577-triangle trunk, frozen wind, 3840 × 2160, 45° vertical field of view and one sample per pixel. Both engines use frustum culling with HZB off. The cameras look toward the same point from yaw 132°, pitch −5.46° at 41 m and −2.235° at 100 m. UE uses its Preserve Area triangle assembly at 1 px and draws zero bricks. The custom camera uses direct submission. Each engine is measured against its own finest shared-input mask. A custom result passes when it meets or exceeds UE’s occupied-pixel intersection over union (IoU) and has at most UE’s fraction of outline pixels farther than one Euclidean pixel from the other outline, counting both directions and the openings inside the crown. These are per-pose settings. [Sweep and reference scores](../build/oak-branch-step-final/sweep.csv) · [Unreal tree counters](../build/oak-triangles-matched/unreal-counts.csv)

| Pose and build | Pixel setting | Camera tree triangles | Camera GPU ms | Occupied IoU | Edges beyond 1 px |
| --- | ---: | ---: | ---: | ---: | ---: |
| 41 m · current | 1.02610 | 3,641,369 | 0.767 | 97.125% | 4.8552% |
| 41 m · same bough, grid | 1.02610 | 3,635,187 | 0.714 | 97.125% | 4.8547% |
| 41 m · connected | 1.02211 | 3,632,961 | 0.715 | 97.128% | 4.8539% |
| 41 m · Nanite | 1 | 2,513,534 | 0.562–0.564 | 97.021% | 4.8557% |
| 100 m · current | 0.49034 | 2,431,143 | 0.667 | 95.892% | 6.154% |
| 100 m · same bough, grid | 0.49034 | 2,431,215 | 0.567 | 95.890% | 6.122% |
| 100 m · connected | 0.49018 | 2,380,803 | 0.566 | 95.890% | 6.090% |
| 100 m · Nanite | 1 | 1,073,068 | 0.478–0.519 | 95.889% | 6.702% |

Each custom row uses the cheapest passing cut sampled for that build. The connected pilot saves **8,408 triangles (0.23%) at 41 m and 50,340 (2.07%) at 100 m** against the current renderer. It still draws **44.5% and 121.9% more triangles** than Nanite. At 41 m, matching the whole crown requires a slightly tighter setting with the connected cut, which spends 27,968 more triangles on leaves. [Selected cuts](../build/oak-branch-step-final/best.csv) · [Shaded timing samples](../build/oak-branch-step-final/timings.csv)

At identical passing settings of 1.02211 and 0.49018, the grid and connected builds have the same bough boundary, surrounding grid parents, leaf geometry and trunk. This isolates the new wood geometry. The 41 m bough goes from **99,057 to 67,151 triangles**, saving **31,906 (32.2%)**. All 371 placements select the 181-triangle connected spray. At 100 m it goes from **113,622 to 60,844**, saving **52,778 (46.5%)** with the 164-triangle spray. Whole-tree shaded GPU time goes from **0.717 to 0.715 ms** and **0.567 to 0.566 ms**, respectively. Moving the boundary alone saves 6,182 triangles at 41 m and costs 72 at 100 m. [Same-setting comparison](../build/oak-branch-step-final/common.csv) · [Observed bough selections](../build/oak-branch-step-final/diagnose.log)

The custom GPU numbers are medians of four 61-frame shaded-camera medians on the RTX 5090, alternating order. The UE ranges are two fresh run medians of the per-frame sum of Nanite visibility and Nanite base pass, trimming eight starting and five ending samples from each 65-sample stream. The custom shaded camera includes the ground and sky. UE profiles an isolated oak and exposes different GPU pass boundaries. The triangle and image comparisons share a target. The GPU columns describe the workload on each engine and do not establish whole-frame speed parity. [Custom runs](../build/oak-branch-step-final/timings.csv) · [UE 41 m profile](../build/unreal-nanite-oak/Saved/Profiling/CSV/branchstep-q41-r1.csv) · [UE 100 m profile](../build/unreal-nanite-oak/Saved/Profiling/CSV/branchstep-q100-r1.csv)

### Most of the GPU gain comes from vertex layout

The same-boundary grid control delivers almost all of the apparent speedup over the current renderer. Rebuilding crossing grid parents lays out wood vertices in local first-use order. To test this, we reordered only the current wood vertex buffer and remapped indices, verifying identical vertex attributes at every index and the same cuts. At the common settings, the four-run median fell **0.757 → 0.696 ms at 41 m** and **0.666 → 0.577 ms at 100 m**. Sorting vertices globally in the connected asset moved time the other way, from 0.701 to 0.754 ms and 0.566 to 0.658 ms. Packing is a separate optimization that works with today's triangles. These times are from the dedicated layout replay. [All layout runs](../build/oak-branch-step-final/layout.csv) · [Attribute-equivalence check](../build/oak-branch-step-final/layout.py)

## What the canopy hides

### The same wood-only bough in both engines

We built a fresh UE Preserve Area assembly using the same 371 transforms and 950 wood triangles per spray. One transformed spray supplies the static-mesh root and the other 370 use the shared assembly part. The root remains at the tree’s world origin so both bench cameras remain exactly where they were on the complete tree. UE’s finest exposed reference and the custom reference agree on 95.45% of occupied pixels at 41 m and 95.16% at 100 m. The 100 m reference covers only 2,717 pixels in UE and 2,730 in custom. Each implementation is scored against its own reference to avoid charging those raster and asset differences to simplification. The UE profiles and counters here come from the separate wood-only assembly. They cannot be used to assign UE’s complete-tree triangles or milliseconds to a bough. [Assembly recipe](../build/unreal-nanite-oak/branchstep_bough.py) · [Reference agreement](../build/oak-branch-step-final/iso-ue.csv)

| Isolated bough | Setting | Wood triangles | Shaded camera GPU ms | Occupied IoU | Edges beyond 1 px |
| --- | ---: | ---: | ---: | ---: | ---: |
| 41 m · grid | 0.3875 | 191,856 | 0.0874 | 87.46% | 2.828% |
| 41 m · connected | 0.4100 | 140,137 | 0.0842 | 88.38% | 2.740% |
| 41 m · Nanite | 1 | 93,820 | 0.433–0.434 | 82.30% | 2.951% |
| 100 m · grid | 0.8000 | 67,994 | 0.0727 | 53.77% | 15.601% |
| 100 m · connected | 0.3425 | 68,546 | 0.0727 | 46.91% | 15.393% |
| 100 m · Nanite | 1 | 24,424 | 0.381–0.406 | 46.55% | 16.245% |

Here each custom arm independently takes its cheapest sampled automatic cut meeting both local Nanite gates. The 41 m connected cut mixes 309 and 431 triangles per spray and saves **51,719 triangles and 0.0032 ms** versus the grid. At 100 m it costs **552 more triangles** with effectively the same time. Nanite uses 33.1% fewer triangles than our matched connected wood at 41 m and 64.4% fewer at 100 m. Its 100 m bough also allows much more outline change than its 41 m bough. These small isolated workloads accentuate fixed GPU costs and the engines’ different pass scopes. [Custom sweep](../build/oak-branch-step-final/iso-sweep.csv) · [Shaded timings](../build/oak-branch-step-final/iso-timing.csv) · [UE 41 m counters](../build/oak-branch-step-final/ue-isolated-stats-41.png) · [UE 100 m counters](../build/oak-branch-step-final/ue-isolated-stats-100.png) · [UE profile jobs](../build/unreal-nanite-oak/branchstep-isolated-jobs.json)

### Requiring both the bough and the crown to pass

For a diagnostic in the complete tree, we gave this bough the tighter local allowance identified above and swept the rest of the crown as usual. This uses a fixed per-pose multiplier on the bough’s errors and renders the real hierarchy in one camera pass. At the calibration settings the isolated masks were pixel-identical to the passing wood-only cuts. Both sets of Nanite silhouette thresholds also pass in the selected rows below. The native pilot has the ordinary single global budget. These measured multipliers show the value and cost of a local acceptance rule. [All simultaneous checks](../build/oak-branch-step-final/dual-sweep.csv) · [Shaded runs](../build/oak-branch-step-final/dual-timing.csv)

| Connected pilot | Whole-tree triangles | GPU ms | Crown IoU / edge misses | Bough triangles | Bough IoU / edge misses |
| --- | ---: | ---: | ---: | ---: | ---: |
| 41 m, 1.02785 px | 3,663,197 | 0.708 | 97.138% / 4.818% | 140,137 | 88.38% / 2.740% |
| 100 m, 0.49003 px | 2,390,176 | 0.561 | 95.893% / 6.083% | 68,546 | 46.91% / 15.393% |

Relative to the production tree calibrated on its crown, this stricter pilot draws **21,828 more triangles at 41 m** and **40,967 fewer at 100 m**. Production’s whole-tree rows are crown-matched controls. The grid and connected rows in the wood-only table are the direct comparison at equal local quality. A faster raw time in the stricter whole-tree pilot also includes the wood vertex-layout difference and timing-run variation.

### The bough selected for the whole-crown benchmark

We rendered the bough's spray wood alone at the same world positions, against all 352,450 shared-input wood triangles. At the matched whole-tree settings, the D3D camera shows the following. [All six isolated views](../build/oak-branch-step-final/isolated.csv) · [4K capture crops](../build/oak-branch-step-final/bough-crops.png)

| Pose | Grid → connected occupied IoU | Grid → connected edges beyond 1 px |
| --- | ---: | ---: |
| 41 m, benchmark yaw | 61.81% → 45.49% | 11.25% → 14.01% |
| 41 m, held-out yaw 90° | 61.22% → 45.26% | 11.86% → 14.93% |
| 41 m, held-out yaw 180° | 81.34% → 43.22% | 4.95% → 14.43% |
| 100 m, benchmark yaw | 80.79% → 43.36% | 5.63% → 16.88% |
| 100 m, held-out yaw 90° | 81.82% → 41.18% | 5.38% → 19.88% |
| 100 m, held-out yaw 180° | 82.43% → 42.57% | 5.05% → 17.38% |

An independent triangle raster confirmed the 100 m selections. On pixels covered by both the source and the selected cut, median first-hit depth error grew from 0.12 to 2.55 mm and p95 grew from **6.8 mm to 1.22 m**. A missed foreground twig can expose another twig much farther back. Looking at those same camera cuts from the sun direction, p95 grew from 6.8 mm to 1.86 m. This tests caster shape from another direction. Actual receiver shadow quality and moving-wind transitions remain unmeasured. The 41 m grid descends to individual sprays. Its quality figures above come directly from D3D. [Depth and normals](../build/oak-branch-step-final/depth.csv)

We forced every connected triangle budget in the two benchmark poses. The 196-triangle version reached only 48.0% and 49.2% wood IoU. The 309-triangle version reached 76.9% and 77.6%, costing 114,639 triangles on this bough. At 100 m it already costs more than the grid and still loses both local outline measures. The 431-triangle version reached 96.6% and 96.9% but costs 159,901 triangles. **None of the eight uniform fitted cuts both saved triangles and preserved the grid's exposed-bough quality in either benchmark pose.** Forcing the cheapest 116-triangle cut also failed the whole-crown gate at both distances. [Budget and exposed-quality sweep](../build/oak-branch-step-final/finer.csv) · [Forced whole-crown controls](../build/oak-branch-step-final/forced.csv)

## Where the triangle gap comes from

1. **The pre-grid test did recover geometry the post-grid repair could not.** The authored 15,240-triangle spray has 61 edge-connected wood pieces. Its 950-triangle shared input has 93, including 42 pieces of four triangles or fewer. Applying a uniform reduction ratio to these fragments produced 259–355 triangles per spray without a useful automatic selection. Giving individual fragments an error allowance left at least 399 triangles in the second bake. Carrying the authored connectivity through LOD 4 and fitting tapered strips or tubes in addition to attribute-aware collapses produced eight complete spray cuts from 116 to 431 triangles. The fit and its measured errors use shared-input vertices and attributes. The higher-detail source supplies branch labels. Both engines receive the same LOD 4 geometry for the matched camera comparison. [Baker and candidate generator](../Tools/BranchConnectedCut.h) · [Intermediate experiment](../build/oak-branch-step-adaptive/components.json) · [Final bake output](../build/oak-branch-step-final/build.log)
2. **A surface-distance budget alone accepts the wrong thin shapes.** A strip can pass close to the original surface but present a very different projected width when rotated. The proxy can also cover a gap between disconnected LOD 4 slivers belonging to the same authored stem. Source endpoints, normals, UVs, and bidirectional samples do not ensure coverage of a one-pixel twig or the same first visible hit. The held-out views and the depth distribution expose this failure. A modular coverage/depth evaluator should rank the existing cut, fitted cuts and subsequent joint leaf/wood candidates against the same local references. Protect the projected area and openings of exposed wood across directions. Use independent camera and light cuts where foliage genuinely hides wood.
3. **Our hierarchy still has too few economical replacements.** The new ranges cover one authored bough with progressively smaller groups, down to 23–24 placements. All placements in a selected range use one reusable spray template. Leaves and wood still select separately. UE's local source uses shared-edge weights and spatial locality to group child clusters, simplifies their merged geometry while protecting external edges, partitions the result into replacement clusters, and repeats with inherited group error. Its Preserve Area option can redistribute area over surviving edges. Assembly parts are reused and can then participate in higher-level group reduction. Our UE benchmark puts both materials in one spray mesh. This gives UE opportunities our bake does not offer. Per-material UE counters for the complete tree are unavailable, so the source comparison identifies mechanisms, not an exact attribution of the gap. [Nanite graph partition](../../UnrealEngine/Engine/Source/Developer/NaniteBuilder/Private/ClusterDAG.cpp#L813) · [Parent reduction](../../UnrealEngine/Engine/Source/Developer/NaniteBuilder/Private/ClusterDAG.cpp#L1553) · [Edge locks and area preservation](../../UnrealEngine/Engine/Source/Developer/NaniteBuilder/Private/Cluster.cpp#L1025) · [Assembly build](../../UnrealEngine/Engine/Source/Developer/NaniteBuilder/Private/NaniteAssemblyBuild.cpp) · [Matched UE assembly recipe](../build/unreal-nanite-oak/build_assemblies.py)
4. **Relaxing the existing threshold hits an asset floor.** At 100 m the current renderer draws the same **1,478,342 triangles from 1.25 through 4 px**. That comprises 326,370 leaves, 863,617 spray wood and 288,355 trunk. It is already 405,274 triangles over Nanite’s matched 1,073,068 and its IoU has fallen to 94.43% against Nanite’s 95.89%. At the actual matched current setting, leaves and trunk alone consume 980,178 triangles, leaving 92,890 for all woody sprays to equal UE's total. The 41 m current renderer can get below UE's triangle count at 1.25 px but misses both silhouette gates. There is no global threshold that repairs those missing representations in these measurements. [Relaxed threshold sweep](../build/oak-branch-step-final/relaxed.csv)

## Milestones and decision

Step 1 was the first opportunity for a substantial whole-tree saving because fixing the shared leaf spray affects 4,730 placements. It delivered 525,837 triangles at 41 m and only 69,943 at 24 m. Step 2 tested whether the restoration cost was locally recoverable. Its two exposed-quality-preserving repairs saved eight and ten triangles. **Step 3 was the point to expect substantial savings on the tested bough at comparable exposed quality. It met that expectation at 41 m and missed it at 100 m.** Even a free wood cut on this bough could recover only about 0.10–0.11M triangles at the original whole-crown settings. Whole-tree parity was never a realistic expectation for one of the tree's boughs. [Step 1 results](OAK_LEAF_SPRAY_24_41.md) · [Step 2 results](OAK_TWIG_REPAIR_41_100.md)

Keep the grid as the accepted control. Next, validate a joint leaf/wood replacement on this bough, weighting multi-view coverage and foreground depth before storing a cut. Keep enough local choices to refine the exposed side of a bough independently of its screened interior. Compare the exact same region at matched local and whole-crown quality. Optimize vertex layout independently on production geometry. **Defer step 4's voxel bake until that evaluator can judge both triangles and bricks fairly.** The matched Nanite assembly achieved its counts using triangles alone.

**Source access was verified on the local UE 5.8.4 checkout.** We read the triangle builder, mesh simplifier, assembly code and projected-error shader directly. The new fitter was implemented independently. Incorporating actual Epic code is subject to the project’s Unreal agreement. In the standard EULA, §2 covers private use and modification, §§4–5 cover distribution and §6(d) specifically addresses Developer/Editor modules such as the builder. [Local build version](../../UnrealEngine/Engine/Build/Build.version) · [Projected-error shader](../../UnrealEngine/Engine/Shaders/Private/Nanite/NaniteClusterCulling.usf) · [Epic’s Unreal Engine EULA](https://www.unrealengine.com/eula/unreal)

## Build and inspect

From the CustomGameEngine repository root in PowerShell, build and open the opt-in pilot. The regular Release viewer is the production control.

```powershell
$cmake = (Select-String '^CMAKE_COMMAND:INTERNAL=(.*)' build/CMakeCache.txt).Matches.Groups[1].Value
& $cmake --build build --config Release --target oak_branch_cut_pilot --parallel 6
.\build\Release\branch-cut\CustomGameEngine.exe --review-capture --scene forked-oak --distance 100 --yaw-degrees 132 --pitch-degrees -2.235 --msaa 1 --wind-strength 0 --lod auto --spray-mode hierarchy --tree-submit direct --spray-pixel-error 1
```

For 41 m use `--distance 41 --pitch-degrees -5.46`. The pilot uses the full authored source as its error reference. At the default 1 px the current → pilot tree counts are **4,000,805 → 4,031,057 at 41 m** and **1,488,389 → 1,463,933 at 100 m**. To view the exact shared-input crown-only cuts, use `.\build\oak-branch-step-final\cut\CustomGameEngine.exe` with `--spray-pixel-error 1.02211` at 41 m or `0.49018` at 100 m. To see the cuts passing both local and crown tests, use the `dual41` or `dual100` viewer in the same directory at 1.02785 or 0.49003 respectively. The sibling `grid` and `current` directories contain their matched controls. `--review-capture` opens the interactive viewer at that pose.

The native pilot hierarchy matches the benchmark input bake byte for byte. The experimental sidecar is 237.7 MiB compared with 148.4 MiB in production. Alternatives are expanded across placements in this pilot, so its disk size is another reason to keep it opt-in. The batched replay produced identical whole-crown masks at both distances and submitted six fewer leaf triangles at 100 m than the measured direct path. The Release build and both registered CTest suites passed. [Camera verification](../build/oak-branch-step-final/replay.log) · [Test output](../build/oak-branch-step-final/ctest.log)
