# A mixed triangle and brick cut for one exposed oak spray group

The experiment uses 12 spray placements on bough 7 of the forked white oak. We ranked the full-size groups by how many of their source pixels were first visible in the parent bough at 100 m. Group 4241 led with 869 visible pixels out of 1,017 isolated pixels. All cuts in both engines use those same placements. The quoted exposure fraction is relative to the parent bough. [Placement IDs](../build/oak-mixed-spray/group.json) · [Exposure ranking](../build/oak-mixed-spray/exposure.csv)

## Motion and quality

The camera travelled from 50 to 100 m in one-metre steps at 30 frames per second and returned over the same positions. We tested yaw 132° and a second heading of 23° after freezing the bake. Each leg contains 51 frames including the endpoints. Both engines used 3840 × 2160, a 45° vertical field of view, pitch −2.235°, frozen wind, frustum culling and no HZB. Native UE 5.8 ran its Voxelize asset at the default 1 px setting. The isolated triangle baseline uses the existing reusable spray policy. [Native motion capture](../Tools/CaptureMixedOakMotion.py) · [D3D fixture and bake](../Tools/MixedOakSpray.py)

For each pose, a cut passes if its foreground intersection-over-union equals or exceeds the native Voxelize score and its symmetric one-pixel outline miss rate is at or below the native score. We checked both each renderer’s own finest triangle mask and a shared native finest triangle mask. For motion, we compared the set of pixels that changed since the preceding frame to the corresponding changes in the finest reference, normalized by the union of the four foreground masks. The reversal repeats the endpoint, and the temporal summary counts the 50 transitions within each leg. [Primary frame measurements](../build/oak-mixed-spray/quality.csv) · [Second heading](../build/oak-mixed-spray/yaw-23/quality.csv)

The baker ranked 540 source leaves by first-visible area and outline contribution at three training headings of 90°, 165° and 225°. It kept 464 leaves as a 964-triangle residual, sampled the other leaves into 2.9 cm cells, and ranked cells and bricks by first-visible leaf material, depth, uncovered pixels and extra coverage. The final 28 bricks contain 89 occupied cells. Twig tips and branches use a 528-triangle geometric cut. Both materials switch together when the candidate’s 2.1 cm selection error projects below one pixel for the group bounds. Shadows use the triangle fallback. The initial half-leaf residual with 44 bricks missed the default voxel outline target on the primary far frames. [Final bake](../build/oak-mixed-spray/bake.json) · [First attempt](../build/oak-mixed-spray/first-fit/quality.csv)

On the primary camera our first bricks appear at 62 m and Nanite's at 57 m. On the second camera they appear at 60 and 55 m. The mixed cut passes both spatial tests on all 204 captured frames and has lower temporal disagreement than default Voxelize on all 200 tested transitions. At the primary heading, p95 temporal disagreement was 22.8% for the existing triangles, 31.4% for the mixed cut, and 56.8% for default Voxelize. At 23° the same measurements were 23.5%, 32.3% and 55.0%. [Primary motion comparison](../build/oak-mixed-spray/motion.gif) · [Second heading](../build/oak-mixed-spray/yaw-23/motion.gif)

### Primary-camera geometry

Cells in the table report surface triangles / bricks. Each brick submits 12 proxy triangles in the D3D renderer and uses a software brick rasterizer in Nanite. The diagnostic removes five degenerate triangles outside the isolated group from the D3D counts.

| Distance | Existing D3D triangles | D3D mixed | D3D coarse triangles | Nanite Voxelize | Nanite Preserve Area |
|---:|---:|---:|---:|---:|---:|
| 50 m | 3,708 / 0 | 3,708 / 0 | 3,708 / 0 | 4,157 / 0 | 5,226 / 0 |
| 75 m | 2,940 / 0 | 1,492 / 28 | 1,644 / 0 | 1,385 / 649 | 5,226 / 0 |
| 100 m | 2,940 / 0 | 1,492 / 28 | 1,644 / 0 | 0 / 375 | 4,718 / 0 |

At 100 m the mixed cut submits 1,828 triangles including proxies, a reduction of 37.8% from 2,940. The additional all-triangle control uses the same switch and twig tier, and draws every leaf from the same coarse triangle template used for the mixed residual. It submits 1,644 triangles. Its primary 100 m IoU and outline miss rate were 0.799 and 1.3%, compared with 0.737 and 3.8% for the mixed cut, 0.812 and 1.2% for the original triangles, and 0.442 and 6.0% for default Voxelize. [Per-distance, per-direction work and silhouettes](../build/oak-mixed-spray/quality.csv)

## Shaded GPU times

The machine used an RTX 5090. D3D ran each out-and-back leg four times per arm with the order varied and 32 warm-up frames at the starting pose. Each reported time is the median across four run medians. The tree time sums separately measured triangle and brick draws on each frame. The camera and full GPU frame columns include progressively more work. On active primary frames the 28-brick draw alone took a median 0.00496 ms. [Primary D3D timestamps](../build/oak-mixed-spray/d3d-gpu.csv) · [Second heading](../build/oak-mixed-spray/yaw-23/d3d-gpu.csv)

| Isolated group | Tree GPU ms | Camera GPU ms | Full GPU frame ms |
|---|---:|---:|---:|
| Existing triangles, 132° | 0.00718 | 0.07810 | 0.08333 |
| Mixed, 132° | 0.01195 | 0.08198 | 0.08720 |
| Coarser triangles, 132° | 0.00715 | 0.07809 | 0.08334 |
| Existing triangles, 23° | 0.00723 | 0.07721 | 0.08245 |
| Mixed, 23° | 0.01175 | 0.07917 | 0.08442 |
| Coarser triangles, 23° | 0.00723 | 0.07752 | 0.08268 |

Native Unreal timed its visibility buffer and Nanite base pass together on the moving, isolated camera with shadows off. Default Voxelize took 0.372 ms at 132° and 0.370 ms at 23°. Native Preserve Area took 0.364 and 0.363 ms. These measurements include UE's pass overhead for rendering a single tiny group. The D3D camera uses the lit oak scene, so the useful GPU comparisons are between variants within the same engine. Unreal returned an access violation during shutdown on some runs after logging completion. All 510 expected native images per heading and all timing sample sets were checked before scoring. [Primary native timestamps](../build/oak-mixed-spray/native-gpu.csv) · [Second heading](../build/oak-mixed-spray/yaw-23/native-gpu.csv)

In the complete oak on the primary path, changing only this group gave the following medians. The shadow triangle counts matched at every paired camera position. Removing the brick sidecar from the mixed hierarchy reproduced the baseline mask on all 8,294,400 pixels in a separate 100 m fallback check. [Full-oak per-frame GPU results](../build/oak-mixed-spray/crown/cost.csv)

| Complete oak | Tree GPU ms | Camera GPU ms | Full GPU frame ms | Submitted triangles at 100 m |
|---|---:|---:|---:|---:|
| Existing triangles | 0.31218 | 0.38502 | 0.52006 | 1,425,976 |
| One mixed group | 0.31840 | 0.39090 | 0.52590 | 1,424,864 |
| One coarser triangle group | 0.31173 | 0.38463 | 0.51990 | 1,424,680 |

**Prefer the coarser triangle tier for this group.** Replacing the last 152 surface triangles with 28 bricks added 336 box triangles and a separate GPU draw. The all-triangle control passed the same spatial and motion quality comparisons and had a closer silhouette. Its whole-oak frame time was effectively tied with the existing baseline. A shared brick raster pass or a group that can retire substantially more triangles per brick would change the economics of a subsequent test.

## Build and view

From the engine checkout with the staged reusable oak and Nanite benchmark fixtures, run the following in PowerShell.

```powershell
.\Tools\BuildOakMixedSpray.ps1 -View -Distance 100
```

Add `-Crown` to see the selected group in the full oak. Use `-Arm tri` for the existing triangles or `-Arm coarse` for the additional triangle-only control. The default arm opens the mixed cut. Use `-Benchmark` to rebuild the candidate and run both native and D3D camera paths, the shaded GPU timings and the full-oak timing control. The benchmark writes to `build/oak-mixed-spray`. The review and shaded D3D motion runs inherit the oak’s canopy-filtered sky, ground bounce and leaf transmission. [Build script](../Tools/BuildOakMixedSpray.ps1)
