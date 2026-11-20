"""Create the two-material, 371-placement Nanite bough control in the bench project."""
import json
import os
from pathlib import Path
import unreal as u

stage=Path(u.Paths.convert_relative_path_to_full(u.Paths.project_dir()))
out=stage.parent/"oak-assembly-step"
bough=int(os.environ.get("OAK_REUSABLE_BOUGH","7"))
asset_path=f"/Game/NaniteOakBench/AssemblyStep{bough:02d}Joint"
voxelize=os.environ.get("OAK_VOXEL_CONTROL")=="1"
if voxelize:
    asset_path+="Voxels"
if "OAK_REUSABLE_BOUGH" in os.environ:
    data=json.loads((out/f"reusable/bough{bough}-ue.json").read_text())
else:
    data=dict(root=json.loads((out/"joint-root.json").read_text()),
              others=json.loads((stage/"branchstep-bough-input.json").read_text())["others"])
assets=u.get_editor_subsystem(u.EditorAssetSubsystem)
actors=u.get_editor_subsystem(u.EditorActorSubsystem)
static=u.get_editor_subsystem(u.StaticMeshEditorSubsystem)
V=u.Vector
try:
    if assets.does_asset_exist(asset_path):
        tree=u.load_asset(asset_path)
    else:
        dm=u.DynamicMesh()
        for slot,s in enumerate(data["root"]):
            buf=u.GeometryScriptSimpleMeshBuffers()
            fields=dict(vertices=[V(*x) for x in s["vertices"]], normals=[V(*x) for x in s["normals"]],
                        triangles=[u.IntVector(*x) for x in s["triangles"]],uv0=[u.Vector2D(*x) for x in s["uv0"]])
            if "uv1" in s:fields["uv1"]=[u.Vector2D(*x) for x in s["uv1"]]
            buf.set_editor_properties(fields)
            u.GeometryScript_MeshEdits.append_buffers_to_mesh(dm,buf,slot)
        opt=u.GeometryScriptCreateNewStaticMeshAssetOptions()
        opt.set_editor_properties(dict(enable_recompute_tangents=True,enable_nanite=False,enable_collision=False))
        result=u.GeometryScript_NewAssetUtils.create_new_static_mesh_asset_from_mesh(dm,asset_path,opt)
        tree=result[0] if isinstance(result,tuple) else result
    for i,name in enumerate(("/Game/M_Oak_Leaf","/Game/M_Oak_Twig")):tree.set_material(i,u.load_asset(name))
    settings=tree.get_editor_property("nanite_settings")
    preservation=u.NaniteShapePreservation.VOXELIZE if voxelize else u.NaniteShapePreservation.PRESERVE_AREA
    settings.set_editor_properties(dict(enabled=True,shape_preservation=preservation))
    static.set_nanite_settings(tree,settings,True)
    if not settings.get_editor_property("nanite_assembly_data").get_editor_property("parts"):
        actor=actors.spawn_actor_from_class(u.Actor,V(),u.Rotator(),transient=True)
        comp=actor.call_method("AddComponentByClass",args=(u.InstancedStaticMeshComponent,False,u.Transform(),False))
        part=u.load_asset("/Game/NaniteOakBench/Spray_Optimized_"+("Voxels" if voxelize else "Triangles"))
        assert part.get_editor_property("nanite_settings").get_editor_property("shape_preservation")==preservation
        comp.set_static_mesh(part)
        transforms=[]
        for p,q,s in data["others"]:
            t=u.Transform()
            t.set_editor_properties(dict(translation=V(p[0]*100,p[2]*100,p[1]*100),rotation=u.Quat(-q[0],-q[2],-q[1],q[3]),scale3d=V(s[0],s[2],s[1])))
            transforms.append(t)
        comp.add_instances(transforms,False,False)
        assert comp.get_instance_count()==len(data["others"])
        builder=u.NaniteAssemblyStaticMeshBuilder.begin_edit_static_mesh_assembly_build(tree)
        assert builder.add_assembly_parts_from_component(comp)
        assert builder.finish_assembly_build() is not False
        actors.destroy_actor(actor)
    assert tree.get_editor_property("nanite_settings").get_editor_property("nanite_assembly_data").export_text().count("PartIndex=0")==len(data["others"])
    assert assets.save_loaded_asset(tree,only_if_is_dirty=False)
    for name in ("M_Oak_Leaf","M_Oak_Twig"):
        m=u.load_asset("/Game/"+name)
        while isinstance(m,u.MaterialInstance):m=m.get_editor_property("parent")
        u.log(f"OAK_BENCH {name} base material two-sided = {m.get_editor_property('two_sided')}")
    u.log("OAK_BENCH ASSEMBLIES_COMPLETE")
finally:
    u.SystemLibrary.quit_editor()
