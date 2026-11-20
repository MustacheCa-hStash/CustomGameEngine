# Forked white oak bough comparison at 4K

Custom engine Release and Unreal Engine 5.8.4 ran at 3840 × 2160 on the same RTX 5090 with a 45° vertical field of view. The tests use the existing tree, materials, and 4,730 PVE placements. Each renderer uses its existing benchmark lighting, 100% render scale, frozen wind, and a 1 px camera setting. Custom captures use 1× MSAA. Unreal uses the warmed Nanite viewport counters for geometry and a separate offscreen GPU profile with the virtual shadow map cache disabled for timing.

## Preserve Area

Unreal's mesh simplifier tracks area lost by material during cluster reduction. Preserve Area distributes loss back over the surviving open edges of the material with the greatest area loss. The source estimates the displacement from lost area divided by the surviving perimeter and respects locked vertices. This addresses open geometry such as leaf patches and thin foliage strips. [Epic's documentation](https://dev.epicgames.com/documentation/unreal-engine/nanite-technical-details?application_version=5.8)

The bough bake applies the idea to fitted leaf quads. It tests single triangles spanning the leaf's original length and area, with their lateral offsets chosen by distance to the original quad. Both directions are measured, and leaves above the new displacement allowance keep their quads. All selected triangles retain their UVs and original winding. The original sampled leaf error is carried into the parent along with the new displacement. This is a per-leaf area fit for the existing bough recipe. A future cluster bake could redistribute area after removing leaves by measuring local patch coverage from multiple directions.

The wood recipe starts from the 422-triangle spray wood LOD. The bake simplifies across the combined wood, rejects proxy triangles beyond the sampled allowance, and restores source triangles where samples would otherwise fall outside that allowance. Direct measurement found a 10.3 cm source-to-proxy deviation on an early candidate using a 1.2 cm grid. The final recipe samples both directions on the finished mesh and adds the prior spray error. The bough's 1 px test uses the larger of the leaf and wood errors. [Bough bake](../Tools/BakeSprayBough.cpp) · [Shared distance measurements](../Tools/SurfaceDistance.h)

| Bough 7 with 371 placements | Leaf triangles | Wood triangles | Total triangles |
| --- | ---: | ---: | ---: |
| Individual coarse sprays | 33,390 | 156,562 | 189,952 |
| Previous bough proxy | 33,390 | 67,352 | 100,742 |
| Area-preserving bough | 25,599 | 66,577 | 92,176 |

At equal triangle count the uncompensated leaf cut retained 82.58% of the fitted surface area. The compensated cut retained 100% by design. At 100 m, visible coverage within the affected crop differed by −0.37%, −0.85%, and −0.04% for the uncompensated cut in the quarter, front, and side views. With compensation the differences were −0.02%, 0.00%, and 0.00%. Each capture uses the same wood mesh and holds bough selection active in both passes. [Matched crops](../build/oak-bough-4k/bough-coverage.png) · [Coverage counts](../build/oak-bough-4k/coverage-4k.csv)

## Render resolution and the switch

The camera test multiplies the projected error by the view's focal length in pixels. At a fixed field of view, focal length in pixels scales with render height. A displacement that spans one pixel at 1920 × 1080 spans two at 3840 × 2160 for the same view. The effective render resolution determines the check. The screen's physical dimensions affect how large the pixels appear to the viewer. Shadows have a separate light-view allowance of 0.75 shadow texels.

On the 80–160 m retreat at 4K with a 2 m step, the new bough switches between 98 and 100 m. The previous bough switches between 142 and 144 m. A 1920 × 1080 control accepted the previous proxy at the same 100 m pose. The normal viewer fits the desktop work area and uses a fixed 3840 × 2160 render buffer. Windowless captures default to 4K as well. [Switch comparison](../build/oak-bough-4k/4k-bough-switch.mp4) · [Projection code](../src/D3D12Renderer.cpp)

## Camera triangles across the test range

The follow-up comparison uses the same LOD 4 spray input, matched culling conditions, and counters from the actual offscreen views. [Matched triangle comparison](OAK_MATCHED_TRIANGLE_COMPARISON.md)

The Unreal column uses the optimized spray and Preserve Area with zero voxel bricks. Its numbers count selected viewport triangles. Custom values count submitted camera triangles. GPU pass timings and color-mask measurements are saved separately. [Custom samples](../build/oak-bough-4k/custom-4k.csv) · [Unreal samples](../build/oak-bough-4k/unreal-4k.csv) · [Native Nanite counters](../build/oak-bough-4k/native-hud-counters.png)

| Distance | Custom previous | Custom area-preserving bough | Unreal optimized triangles |
| ---: | ---: | ---: | ---: |
| 14 m | 53,864,442 | 53,864,442 | 5,309,448 |
| 18 m | 37,361,280 | 37,361,280 | 3,384,264 |
| 24 m | 15,401,320 | 15,401,320 | 2,043,796 |
| 41 m | 9,915,560 | 9,915,560 | 1,691,344 |
| 100 m | 3,227,340 | 3,129,564 | 177,084 |

The bough saves 97,776 camera triangles at 100 m and saves the same number in the shadow pass at each of the five poses. The camera retains spray detail at 14–41 m. At 41 m, all 4,730 sprays still use the 1,504-triangle leaf level at 4K, which contributes 7,113,920 triangles. At 14 m the leaves contribute 30,632,300 triangles and spray wood contributes 22,426,562. These ranges motivate intermediate per-spray cuts and finer group culling. The far comparison still needs lower trunk geometry and more parent meshes that replace groups of sprays.

The 100 m custom camera pass stayed around 0.35–0.37 ms across the two runs. In the first run the median shadow pass went from 0.258 to 0.221 ms, and in the second run it went from 0.233 to 0.223 ms. The raw timings include both runs at every pose. Unreal's visibility pass is separately scoped from its base pass and the custom renderer's full camera pass.
