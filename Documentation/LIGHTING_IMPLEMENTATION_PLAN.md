# Oak lighting implementation plan

## Review the current renderer first

The forked white oak reads reasonably from the lit side. From underneath and on the shaded side, foliage and twigs merge into a dark, noisy interior. The goal is to recover readable foliage patches and depth while preserving canopy shade and the authored tree. Start at leaf scale so we can identify the material, shadowing, and lighting contributions before evaluating the whole crown.

The fixture uses three copies of the modeled 18 cm white oak leaf at LOD 0, with the original geometry, UVs, and base color. Their different tilts and staggered positions leave exposed and overlapping regions. A short opaque twig crosses the bottom layer. The fixture sets the imported material's legacy scalar transmission to `0.45`, the value used by Forked White Oak. The renderer uses the existing base color and mesh normals for both faces. The supplied normal and transmission textures are available as future material inputs.

- [Nine-view comparison sheet](../build/white-oak-leaf-lighting-review/leaf-stack-comparison.png). Each row is one layer. Columns are above, side, and below. Each layer image is a pixel crop from a shared camera plate, so adjacent geometry and its shadows remain in the scene.
- Full plates for [above](../build/white-oak-leaf-lighting-review/stack-above.png), [side](../build/white-oak-leaf-lighting-review/stack-side.png), and [below](../build/white-oak-leaf-lighting-review/stack-below.png). The nine individual crops are in the same directory as `top-*`, `middle-*`, and `bottom-*`.
- [Leaf shadow reception comparison](../build/white-oak-leaf-lighting-review/leaf-shadow-reception-comparison.png). The current renderer shades much of the middle leaf and part of the bottom leaf. Turning off leaf shadow reception recovers direct light in those areas while keeping the camera, light, casters, and background fixed. This confirms the fixture exercises mutual shadowing.
- [Fixture GLB](../build/white-oak-leaf-lighting-review/WhiteOak-LeafLightingFixture.glb), [capture settings and binary fingerprint](../build/white-oak-leaf-lighting-review/capture-settings.json), and [capture script](../Tools/CaptureLeafLightingBaseline.py).

The baseline uses the stock Release renderer and its fixed display transform. There is no adjustable exposure in this path. All three views use 3840 × 2560, 4× MSAA, zero wind, a 0.50 m orbit distance, yaw 4°, and sun azimuth −90° / elevation 49°. Camera pitches are 73°, 2°, and −73°. The darker ground behind the overhead views and bright sky behind the underside views can affect perceived contrast, so compare equivalent leaf regions as well as the overall image.

To orbit the fixture from any directory in PowerShell, open the generated GLB with the viewer. For a manual comparison, set wind strength to zero and match the sun angles above in Scene Controls.

```powershell
$engineRoot = 'C:\Users\samee\GitProjects\CustomGameEngine'
& "$engineRoot\build\Release\CustomGameEngine.exe" "$engineRoot\build\white-oak-leaf-lighting-review\WhiteOak-LeafLightingFixture.glb"
```

To recapture, use Python 3.11 with Pillow and supply a new output directory. The script locates the source GLB and Release viewer relative to its own path. Use `--engine` to select a different compatible build. Save the current plates before replacing the baseline executable with a lighting build.

```powershell
$engineRoot = 'C:\Users\samee\GitProjects\CustomGameEngine'
$capturePython = 'C:\Users\samee\AppData\Local\Programs\Python\Python311\python.exe'
& $capturePython "$engineRoot\Tools\CaptureLeafLightingBaseline.py" "$engineRoot\build\white-oak-leaf-lighting-repeat"
```

## Proposed implementation after baseline review

1. **Linear HDR and fixed exposure.** Keep the forward camera pass. Read color textures as linear RGB and accumulate the mesh, voxel, and sky shading in an `RGBA16F` scene target. Resolve 4× MSAA in linear HDR before a fullscreen pass applies manual exposure, tone mapping, and one sRGB conversion. Use the same display pass for the SDR window and PNG captures, draw UI afterward, and keep geometry masks and data diagnostics on a path that preserves their encoded values. First recapture the legacy lighting terms through the new pipeline. Check for double encoding, highlight clipping, and 1× / 4× GPU time and bandwidth costs. [Current camera pass](../src/D3D12Renderer.cpp#L2436), [D3D12 resolve](https://learn.microsoft.com/en-us/windows/win32/api/d3d12/nf-d3d12-id3d12graphicscommandlist-resolvesubresource)
2. **Directional diffuse skylight.** Use a scene-linear HDR sky dome with the sun disk handled by the existing directional light. Derive both the visible sky and diffuse lighting from that source. Project the sky into nine RGB spherical harmonic coefficients once per sky update and evaluate diffuse irradiance by world-space surface normal. Share the evaluation between mesh and voxel shading. Start with open-sky irradiance and validate orientation in sky-only captures. Local canopy visibility and ground bounce belong in the subsequent world-space lighting system. An analytic sky can later supply the same data for moving time of day. [Diffuse environment lighting](https://aperture.stanford.edu/papers/envmap/)
3. **Two-sided leaf response.** Give the leaf separate diffuse reflectance and transmission colors with a shared incoming-light budget. Evaluate reflected illumination from the visible side and transmission from the opposite side for both sun and sky. Keep authored folds and UVs. Test a thickness or transmission mask if reference views show that thinner areas and veins need different response. Add debug views for direct sun, sky diffuse, transmission, and shadow visibility. Compare sun-only, sky-only, and combined captures against the same fixture rendered offline at matched poses and exposure.
4. **Transmission through multiple leaves.** The current shader shades the *receiving* leaf with a backlighting scalar, then multiplies its direct contribution by ordinary shadow visibility. The depth shadow map blocks the direct beam at the first opaque leaf surface. If the offline comparison shows a meaningful amount of colored light reaching the middle and bottom layers through the upper leaf, prototype depth-aware shadow transmittance. Verify the opaque twig remains an opaque blocker and measure cost on a grove before committing to a technique. [Current leaf shader](../shaders/Mesh.hlsl#L310), [Deep shadow maps](https://www.graphics.stanford.edu/papers/deepshadows/)

## Later forest lighting

The leading GI option is a world-space diffuse probe field with visibility-aware interpolation. Update it with bounded software tracing against terrain, simplified wood, and partial-coverage canopy proxies. Let meshes and render voxels sample the same lighting interface. Keep the indirect-lighting representation independent of visible foliage LOD. Crytek-style voxel GI is a candidate if the probe field loses too much canopy depth at the target GPU budget. Consider a short-range screen-space pass if matched renders show a local detail gap after the world-space lighting is in place. Judge quality on fixed-exposure oak views and performance on a grove walk. [Probe GI](https://jcgt.org/published/0008/02/01/), [Crytek SVOGI](https://www.cryengine.com/docs/static/engines/cryengine-5/categories/23756816/pages/25535599)

For the main chat, start from this plan and the nine-view sheet. The current decision is whether the baseline exposes enough of the top, middle, and bottom layers to use it as the lighting benchmark. HDR, directional skylight, and the revised material are proposed follow-up passes.
