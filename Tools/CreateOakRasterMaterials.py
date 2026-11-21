"""Create constant unlit counterparts for the standalone Nanite raster benchmark."""
import unreal

folder = "/Game/NaniteOakBench"
tree = unreal.load_asset(folder + "/Tree_Optimized_Triangles")
library = unreal.MaterialEditingLibrary
assets = unreal.AssetToolsHelpers.get_asset_tools()
for slot, entry in enumerate(tree.get_editor_property("static_materials")):
    source = entry.get_editor_property("material_interface")
    base = source.get_base_material()
    # The bark instance overrides a masked master with opaque blending.
    two_sided = base.get_editor_property("two_sided")
    blend = base.get_editor_property("blend_mode")
    chain = []
    parent = source
    while isinstance(parent, unreal.MaterialInstance):
        chain.append(parent.get_editor_property("base_property_overrides"))
        parent = parent.get_editor_property("parent")
    for overrides in reversed(chain):
        if overrides.get_editor_property("override_blend_mode"):
            blend = overrides.get_editor_property("blend_mode")
        if overrides.get_editor_property("override_two_sided"):
            two_sided = overrides.get_editor_property("two_sided")
    if blend != unreal.BlendMode.BLEND_OPAQUE:
        raise RuntimeError(f"Cannot replace opacity of {source.get_path_name()} with a constant")
    name = f"RasterFlatSlot{slot}"
    path = f"{folder}/{name}"
    flat = unreal.load_asset(path) if unreal.EditorAssetLibrary.does_asset_exist(path) else None
    if not flat:
        flat = assets.create_asset(name, folder, unreal.Material, unreal.MaterialFactoryNew())
    library.delete_all_material_expressions(flat)
    flat.set_editor_property("shading_model", unreal.MaterialShadingModel.MSM_UNLIT)
    flat.set_editor_property("two_sided", two_sided)
    library.set_material_usage(flat, unreal.MaterialUsage.MATUSAGE_NANITE)
    color = library.create_material_expression(flat, unreal.MaterialExpressionConstant3Vector)
    color.set_editor_property("constant", unreal.LinearColor(.15, .55, .32, 1))
    library.connect_material_property(color, "", unreal.MaterialProperty.MP_EMISSIVE_COLOR)
    library.recompile_material(flat)
    unreal.EditorAssetLibrary.save_loaded_asset(flat, only_if_is_dirty=False)
    unreal.log(f"OakRaster slot {slot} {source.get_path_name()} two-sided {two_sided}")
unreal.log("OakRaster MATERIALS_COMPLETE")
