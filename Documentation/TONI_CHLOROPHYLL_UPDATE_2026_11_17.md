# Chlorophyll Engine showcase audit update

Reviewed November 17, 2026. This supplements the [earlier forest audit](../../FirstSettlers/Documentation/FOREST_RENDERING_REVIEW.md) and the [engine plan](ENGINE_PLAN.md). The source is Toni Macaroni's *[Chlorophyll Engine] 10M Trees - 15km² Test*, published November 17, 2026. The expanded description was read through YouTube's browser UI. The pinned creator comment says an FAQ will be added to the description. This records the description available before that FAQ. [Video and description](https://www.youtube.com/watch?v=VC_NUxbeA0E) · [FAQ announcement](https://www.youtube.com/watch?v=VC_NUxbeA0E&lc=UgyOXqUPhrjR6hYaNjp4AaABAg)

Visual observations below use only the screenshot supplied by Samee. No video frames were sampled, playback was left paused, and no new renderer benchmarks were run. The temporary reference tab was closed after reading.

## New creator statements

| Topic | What the description establishes |
| --- | --- |
| Engine identity and scale | The engine is named Chlorophyll. Toni reports 10 million trees across 15 km². |
| Aggregate selection | Below a screen-footprint threshold, individual instance data leaves GPU residency. Rendering traverses a tile's voxel hierarchy and draws the tile as an aggregate. |
| Storage and streaming | Unique tile proxies consume disk space and VRAM. Tiles stream in and out as needed. |
| Proxy generation | Proxies currently build on the player's hardware. This reduces shipped data and permits hardware-specific representations, including a possible 2D option on lower-end machines. |
| Cost | Toni reports little added cost apart from shading. The description supplies no matched timings, memory totals, or build times. |
| Environment | Terrain, color, postprocessing, clouds, rivers, cliffs, and paths received changes. Rivers and paths currently use meshes or terrain splats. Procedural paths are future work. |

These are creator-reported implementation details and results. [New showcase description](https://www.youtube.com/watch?v=VC_NUxbeA0E)

The tile description resolves an earlier uncertainty about the extreme-distance proxies. Our previous audit suggested regional aggregation as a possible interpretation. Tile aggregation and GPU instance-data removal are now explicit. The description leaves open the threshold, tile size, hierarchy format, encoding, cache policy, generation time, editing behavior, and the relationship between this aggregate hierarchy and the near-tree CLOD. Its scale claim gives scene population, with submitted geometry and visible workload still unspecified. [New showcase description](https://www.youtube.com/watch?v=VC_NUxbeA0E)

The new description provides no leaf shading formula, shadow technique, GI update algorithm, graphics API, implementation language, or meshlet builder details. Carry the earlier mesh-shader and hybrid-GI statements with their original sources. This upload alone adds no confirmation of those internals. [Earlier research and attribution](ENGINE_PLAN.md#research-behind-the-later-experiments)

## What the supplied leaf screenshot shows

The reference is [Samee's supplied IMG_1407.PNG](C:/Users/samee/Downloads/IMG_1407.PNG). That link points to the original local attachment. Camera, sun, exposure, rendering resolution, and active LOD are unknown.

- Some foreground leaves have bright green areas with readable veins and surface variation. Their shape and orientation remain readable within the crown.
- Dark shapes overlap brighter leaf surfaces, especially on the right. These are consistent with leaves and branches interrupting illumination of other leaves. A still image cannot isolate cast shadows from material variation, surface orientation, or indirect occlusion in every patch.
- Dense regions and branch silhouettes become very dark beside the bright sky. The target includes substantial contrast and open sky gaps. Matching average canopy brightness alone would miss those relationships.

The image is consistent with a combination of leaf transmission, mutual shadowing, surface detail, and environment illumination. It cannot identify a particular subsurface-scattering model, colored shadow transmission, ray tracing, voxel shading, or shadow-map architecture. The screenshot also provides no motion evidence for shimmer, denoising, or LOD stability.

## Implications for our current renderer

Our mesh lighting evaluates `max(N·L, 0) + transmission * max(-N·L, 0)`, multiplies that response by shadow visibility, and adds fixed ambient fill through `baseColor * (0.35 + 0.65 * diffuse * visibility)`. This already allows a backlit leaf to receive a shadow from another leaf. When visibility reaches zero, the direct backlighting term reaches zero too. Modeled leaves write opaque depth in the shadow pass. Their Backlighting setting changes the receiving surface's light response. Light passing through several separate leaves would require an additional transport approximation. [Mesh lighting and shadow pass](../shaders/Mesh.hlsl)

The current baseline has authored leaf color detail, modeled silhouettes, and the tools to isolate direct lighting and received shadows. Modeled Oak Walk also has stabilized sun cascades, boundary blending, a wider comparison filter, and receiver-plane adjustment. Use those existing controls to assess leaf-scale detail before selecting another shadow architecture. The fixed ambient term cannot express variation in sky access or bounced illumination across the canopy. [Current viewer and cascade controls](../README.md#sun-shadow-cascade-preview)

The branch experiments separately demonstrate that a good camera silhouette can coexist with incorrect received shadows. Changing cell-entry receiving to midpoint or replacing triangle casters with our voxel casters did not improve the measured lighting overall. The new image strengthens the case for retaining a triangle lighting reference while evaluating coarse foliage representations. Toni's screenshot provides no evidence that its foreground leaves are being rendered as voxels. [Measured shadow comparisons](NANITE_FOLIAGE_PIPELINE_REVIEW.md#triangle-and-voxel-shadow-caster-comparison)

## Proposed next comparisons

These are planning updates. They add no renderer implementation in this audit.

1. Establish a small leaf-over-leaf fixture using the current oak material. Place a leaf between the sun and a receiving leaf, with a branch crossing part of the receiver. Pause wind and save front-lit, backlit, and grazing views with fixed camera, sun, output resolution, and exposure behavior. Start with source triangles and camera-matched shadow geometry.
2. Record base color, diffuse/backlighting, received-shadow visibility, and final shading. Compare Backlighting 0 and 1 with reception on and off, and casting on and off independently. A sun-visible underside should respond to Backlighting. An intervening caster should create a localized change on the receiving leaf while preserving the exposed portion. Check both MSAA modes. Keep bias values low enough to retain those legitimate overlaps.
3. Inspect projected shadow texel size relative to leaf dimensions, filtering, and normal orientation at those poses. Repeat in the full crown with its production shadow LOD. That comparison will distinguish a material issue from a loss of small caster detail or bias-related shadow loss. Record crown-interior differences even when the ground shadow looks acceptable.
4. Compare directional sky fill and a controlled transmission model against this baseline at fixed brightness. Evaluate leaf orientation, vein detail, and shaded canopy depth together. If light through multiple leaves becomes a demonstrated requirement, test an explicit transmittance approximation with thickness/absorption controls and overlapping-leaf fixtures.
5. Repeat held approach and orbit poses for triangle LOD and voxel candidates. Preserve gaps, depth, backlit response, and mutual shadow placement alongside coverage. Add motion checks when a candidate passes the still comparisons.

For longer views, add a separate aggregate-tile experiment after the branch/crown quality gates. Begin with a few repeated groves and matched individual-instance controls. Measure resident proxy bytes, disk bytes, generation time, streaming stalls, GPU traversal/shading time, and handoff quality. Add stable tile IDs, deterministic recipes, versioned caches, and dirty-tile rebuilds when harvesting, construction, or terrain edits enter that experiment. Keep authoritative tree identities and edits in world state while GPU detail residency changes. These are proposed requirements for our editable game world.

## Revisit after the FAQ

Check for published details on near-leaf geometry and material inputs, transmission and shadow interaction, sky/GI, shadow resolution/filtering, CLOD-to-tile handoff, proxy build/cache costs, runtime edits, hardware, and performance conditions. Compare any new claims with this dated description snapshot before revising the attributed implementation. The current FAQ announcement supplies no answers to these questions. [Creator announcement](https://www.youtube.com/watch?v=VC_NUxbeA0E&lc=UgyOXqUPhrjR6hYaNjp4AaABAg)
