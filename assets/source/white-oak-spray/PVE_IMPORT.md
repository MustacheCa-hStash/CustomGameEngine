# Dense white oak spray for UE 5.8 PVE

`SM_WhiteOak_Spray_Dense.fbx` contains one static mesh with 14 leaves on two shoots, a tapered fork, rounded stalks, and buds. It has 74,966 triangles and two material slots, `Oak_Leaf` and `Oak_Twig`. The pivot is at the cut end of the shared stem. In the intended Unreal frame, local +Z points from the stem toward the tips, local +Y is the upper face, and the mesh measures approximately 38 × 18 × 31 cm along X, Y, and Z.

`WhiteOak_Spray_Dense.blend` has the individual leaves and wood editable at real scale, with all five textures packed. The Blender authoring scene is horizontal for inspection. Use the supplied FBX for placement in PVE.

## Import the mesh and textures

1. Enable **Procedural Vegetation Editor** in Plugins and **Nanite Foliage** under **Project Settings → Rendering**. Restart the editor. [PVE guide](https://dev.epicgames.com/documentation/unreal-engine/procedural-vegetation-editor-pve-in-unreal-engine) · [Nanite Foliage setup](https://dev.epicgames.com/documentation/unreal-engine/nanite-foliage)
2. Import the five PNGs from this folder into the Unreal Content Browser. Use sRGB for the two base-color maps and `T_OakLeaf_Transmission`. For both `_Normal` maps, use **Normalmap** compression, turn sRGB off, and enable **Flip Green Channel** for the supplied +Y maps. Leaf maps are padded to 1024 × 2048 with matching mesh UVs, and the twig maps are 1024 × 1024. [Texture import and dimensions](https://dev.epicgames.com/documentation/unreal-engine/textures-in-unreal-engine) · [Green channel setting](https://dev.epicgames.com/documentation/unreal-engine/API/Runtime/Engine/UTexture)
3. Import `SM_WhiteOak_Spray_Dense.fbx` as a **Static Mesh** at scale **1** with **Import Normals and Tangents**. If the importer shows axis and unit settings, enable scene and unit conversion and leave **Force Front XAxis** off. Keep the import rotation and translation at zero. Use **Replace** for the vertex-color import option to retain the per-leaf tint values. Turn off automatic material creation and texture import if you are assigning the materials below. [FBX import settings](https://dev.epicgames.com/documentation/unreal-engine/fbx-import-options-reference-in-unreal-engine)

## Create the materials

| Static mesh slot | Material setup |
| --- | --- |
| `Oak_Leaf` | Set **Blend Mode** to **Opaque**, **Shading Model** to **Two Sided Foliage**, and check **Two Sided**. Wire `T_OakLeaf_BaseColor` to **Base Color** and `T_OakLeaf_Normal` to **Normal**. Start with **Roughness** `0.61`, **Metallic** `0`, and **Specular** `0.36`. Multiply `T_OakLeaf_Transmission` by a scalar starting at `0.6` and connect the result to **Subsurface Color**. To match the Blender tint variation, multiply base color by **Vertex Color RGB**. |
| `Oak_Twig` | Use **Opaque**, **Default Lit**. Wire `T_OakTwig_BaseColor` to **Base Color** and `T_OakTwig_Normal` to **Normal**. Start with **Roughness** `0.69` and **Metallic** `0`. |

The leaf transmission map is derived from the synthetic backlit reference and its multiplier is an artistic starting value. Twig color and relief are procedural. [Two Sided Foliage shading](https://dev.epicgames.com/documentation/unreal-engine/shading-models-in-unreal-engine#two-sided-foliage)

## Place the spray in PVE

1. Open a broadleaf sample graph if available. Keep the graph that generates the woody host and add the imported mesh as one entry in a **Foliage Palette**.
2. Connect the **Mesh Builder** to the **Foliage Distributor** primary input, and the **Foliage Palette** to its **Foliage** input. Try **Parametric** distribution at low density with a fixed seed. Inspect the first few placements at scale 1. The cut ends should meet the host twigs, and the foliage faces should point as intended. Adjust the Distributor’s **Aim**, **Face**, and **offset along trunk thickness** if needed. Raise placement density toward the outer fine branches to fill the crown.
3. Connect the Distributor to **Export**. For the assembly test, choose **Static Mesh**, include foliage, and enable **Create Nanite Foliage**. Save the graph with its seed so the same tree can be regenerated. [PVE nodes and export](https://dev.epicgames.com/documentation/unreal-engine/procedural-vegetation-editor-pve-in-unreal-engine)

The supplied FBX was re-imported into Blender to check its pivot, scale, geometry, and slots. The first placement in Unreal is the check for the target project's FBX import settings and PVE orientation.
