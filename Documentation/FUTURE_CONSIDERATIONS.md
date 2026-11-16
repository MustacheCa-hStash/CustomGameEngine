# Future considerations

## Rendering

- **Modeled foliage shimmer and temporal anti-aliasing (TAA).** Modeled Oak shimmers more than Card Oak during camera motion. Subpixel leaf edges and canopy gaps can flicker even with 4× MSAA. Compare a slow orbit with wind paused and MSAA off and on, then toggle foliage shadow reception to separate coverage flicker from shadow sampling inside the canopy. Consider TAA with subpixel camera jitter and motion-vector reprojection. Test with wind paused first, then account for wind deformation and check for ghosting or blur as gaps open between leaves. For distant foliage, evaluate LODs that preserve apparent canopy coverage and larger gaps.

- **Normal offset bias for shadows.** Evaluate offsetting the receiver's shadow lookup position along its surface normal, with an offset related to the light angle and shadow-map texel size. This may reduce self-shadowing artifacts while retaining contact shadows. Compare it across sun angles on the cube, bark, and both sides of foliage cards, and watch for detached or distorted shadows.

- **Cluster LOD culling.** Modeled Oak Walk has wind-aware camera frustum culling for fixed foliage groups. Use the flat CPU test as the reference for light-frustum group culling or a BVH when a larger workload warrants it. Test camera and light views separately so off-camera groups can still cast visible shadows. Spatial bounds can reject whole subtrees for visibility, and conservative projected parent-error bounds can skip descendants whose coarser representations already meet the LOD target. Evaluate hierarchical depth occlusion separately. Compare selected, visible, and submitted counts and verify that drawing with culling gives the same visible result as the complete cut. [Oak walk results](../README.md#modeled-oak-walk) · [Epic’s speaker notes, *Hierarchical culling* and *Two-Pass Occlusion Culling*](https://advances.realtimerendering.com/s2021/Karis_Nanite_SIGGRAPH_Advances_2021_final.pdf)

- **Draw batching and shadow-specific foliage LOD.** Revisit these optimizations when a full-crown or stand benchmark shows a measurable draw-list, submission, or shadow-pass bottleneck. Use indexed draw records, API calls, CPU draw-list time, and GPU pass time to decide which change to test. Repeat the [Oak Branch shadow calibration](OAK_BRANCH_CLUSTER_LOD_PLAN.md#shadow-lod-calibration) at that scale and when changing the shadow projection or resolution. Check low-sun receiver coverage, wind stability, and the moment a caster crosses the camera frustum before enabling an automatic policy. Use sampled shadow-texel error alongside the image and timing comparisons.

## Viewer tooling

### Headless scene capture

Add a command-line mode to the existing D3D12 viewer that renders one frame to a PNG and exits without opening a window. Use it for repeatable comparisons of engine lighting, foliage, shadows, and MSAA without desktop capture.

1. Share device initialization, asset loading, and the camera and shadow passes with the windowed viewer. In capture mode, render to an offscreen texture instead of a swap-chain buffer. Resolve 4× MSAA to a single-sample texture when enabled.
2. Load a capture preset with the scene, camera, resolution, fixed simulation time, and inspector overrides. Wait for assets to be ready and apply overrides in memory so captures do not write to the viewer's saved settings.
3. Copy the final color texture to a CPU-readable buffer, wait for the GPU fence, and encode a PNG. Account for row pitch and preserve the displayed output's color encoding.

Compare a fixed Cube view with the windowed render in both MSAA modes. In Oak Branch, compare Backlighting at 0 and 1 with the same underside camera, sun, and wind time, and with leaf shadow reception disabled to isolate transmission. Use a fixed Grove shot to check that wind and shadows agree. Live shader reload failure and recovery still require testing within a running process, since a single-frame capture starts fresh.
