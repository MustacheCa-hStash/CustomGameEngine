# Forked white oak per-group triangle LOD at 4K

The Release viewer enables **Spray LOD → Per-group leaf and twig LOD** when automatic LOD uses grouped or bough traversal. Each spray primitive has eight baked spatial groups. The baker reuses the fixed LOD simplifier, measures candidates in both directions against the source and LOD 4, and checks the open rim for leaves. The renderer can select a different cut for each group inside a placement. It batches placement ranges when their projected-error bounds agree on a cut.

```powershell
& 'C:\Users\samee\GitProjects\CustomGameEngine\build\Release\CustomGameEngine.exe' --forked-oak
```

Press **F1** for the inspector and choose **Spray LOD → Color by → LOD level** to see the active cut while orbiting or zooming. Clear **Per-group leaf and twig LOD** to reproduce the whole-spray control at the same pose. Press **M** if the title shows 4× MSAA and you want the single-sample configuration used in the captures.

## Release benchmark

Both modes used the same Release executable, the same 3840×2160 camera poses and lighting, 1× MSAA, frozen wind, grouped frustum culling and the existing bough proxy. The camera used a 1 px projected-error budget and the shadow pass used its existing 0.75 texel budget. Camera counts include the tree plus three sky and ground triangles. Times are median values from the second run of 61 measured frames per configuration on a GeForce RTX 5090. The first run used the opposite mode order. [All timings and counters](../build/oak-local-groups/timings.csv)

| Distance | Whole-spray camera triangles | Per-group camera triangles | Reduction | Camera GPU, ms | Render CPU, ms |
| ---: | ---: | ---: | ---: | ---: | ---: |
| 14 m | 53,864,442 | 39,908,614 | 25.9% | 6.772 → 5.637 | 0.591 → 0.765 |
| 18 m | 37,361,280 | 23,582,106 | 36.9% | 4.360 → 3.181 | 0.570 → 0.734 |
| 24 m | 15,401,320 | 11,477,254 | 25.5% | 1.915 → 1.558 | 0.484 → 0.552 |
| 41 m | 9,915,560 | 6,125,644 | 38.2% | 1.244 → 0.800 | 0.263 → 0.262 |
| 100 m | 3,129,564 | 3,129,564 | 0.0% | 0.399 → 0.398 | 0.053 → 0.054 |

At 24 m the leaf count fell from 7,113,920 to 5,122,114 and the spray wood count fell from 7,481,820 to 5,549,560. At 41 m the leaf count fell from 7,113,920 to 3,324,004 and spray wood held at 1,996,060. Camera draws increased from 6 to 36 at 24 m and from 5 to 18 at 41 m. Both modes submitted 3,129,561 shadow triangles at every distance, and the shadow GPU pass held near 0.20 ms. The 100 m cut uses the existing coarse sprays and bough proxy.

## Coverage and switching

The 24 and 41 m quarter-view masks overlapped by 99.29% and 98.11%. Occupied tree pixels changed by −0.07% and +0.33%, respectively. Overlap is the intersection of the two occupied-pixel masks divided by their union. It includes openings within the crown. At 41 m the front and side views overlapped by 98.29% and 98.11%. The view from under the crown at 18 m overlapped by 99.52%. [24 and 41 m shaded comparison](../build/oak-local-groups/24m-and-41m.png) · [Full-resolution focus renders](../build/oak-local-groups/release/timed)

The moving checks included a 44-frame retreat and orbit from 14 to 100 m and a fixed-angle approach from 50 to 20 m. Local leaf and twig selections changed across the approaches. The lowest same-pose overlap over the retreat was 97.83% at 48 m. The largest occupied-pixel change was 0.38%. The shaded comparison around the largest difference shows fine foliage-edge changes while the main crown outline and exposed limbs stay in place. [Moving comparison](../build/oak-local-groups/4k-group-lod-sweep.mp4) · [Frame-by-frame coverage](../build/oak-local-groups/motion-coverage.csv) · [Coverage detail at 46–50 m](../build/oak-local-groups/motion-detail-review.png)

## Shared-input comparison with Unreal

The staged viewer used the same exported LOD 4 spray meshes that seeded the Unreal assembly. It switched using directly measured LOD 4-relative errors. The Release benchmark above uses the normal source-relative errors and can select finer input geometry close up. Counts below isolate the tree and use the same offscreen 4K poses, nominal 1 px settings and frustum-only culling. Unreal values are from the existing matched captures. The overlap columns compare each result with its own engine's fixed-input geometry mask at the same pose. [Matched setup and counter definitions](OAK_MATCHED_TRIANGLE_COMPARISON.md)

| Distance | Whole-spray triangles | Per-group triangles | Unreal triangles | Per-group overlap | Unreal overlap |
| ---: | ---: | ---: | ---: | ---: | ---: |
| 14 m | 12,109,255 | 11,646,837 | 11,358,109 | 99.98% | 100.00% |
| 18 m | 12,412,997 | 11,312,912 | 10,418,802 | 99.86% | 99.91% |
| 24 m | 12,412,997 | 10,020,142 | 6,516,633 | 99.29% | 98.93% |
| 41 m | 9,915,557 | 5,902,564 | 2,513,534 | 97.95% | 97.02% |
| 100 m | 3,129,561 | 3,129,561 | 1,073,068 | 95.73% | 95.89% |

The 41 m triangle ratio fell from 3.94× to 2.35×. The 100 m ratio held at 2.92×. At 100 m, the 4,359 remaining individual sprays submit 1,839,498 twig triangles. The bough proxy adds 66,577 wood triangles, bringing the spray wood counter to 1,906,075. The original trunk and main bough mesh still contributes 805,577 triangles. [Comparison data](../build/oak-local-groups/comparison.csv)
