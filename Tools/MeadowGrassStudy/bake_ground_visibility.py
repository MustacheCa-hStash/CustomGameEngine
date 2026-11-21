"""Bake short-range hemispherical visibility at the soil in the static grass strip."""
import json
import math
from pathlib import Path
import sys

import bpy


root = Path(__file__).resolve().parents[2]
placements_path, output = map(Path,sys.argv[sys.argv.index("--")+1:])
output = output.resolve()
placements = json.loads(placements_path.read_text())
bpy.ops.wm.read_factory_settings(use_empty=True)
scene = bpy.context.scene
scene.render.engine = "CYCLES"
scene.cycles.samples = 4
scene.cycles.use_denoising = False
try:
    devices = bpy.context.preferences.addons["cycles"].preferences
    devices.compute_device_type = "OPTIX"
    devices.refresh_devices()
    if any(d.type == "OPTIX" for d in devices.devices):
        for d in devices.devices:
            d.use = d.type == "OPTIX"
        scene.cycles.device = "GPU"
except (RuntimeError,TypeError):
    pass

layers = placements.get("layers",[{"asset":str(root/"assets/source/meadow-grass-study/meadow-tuft.glb"),
                                   "placements":placements}])
for layer in layers:
    existing = set(scene.objects)
    bpy.ops.import_scene.gltf(filepath=layer["asset"])
    tuft = bpy.data.collections.new("Shadow geometry")
    for obj in set(scene.objects)-existing:
        for coll in list(obj.users_collection):
            coll.objects.unlink(obj)
        obj.visible_camera = False
        tuft.objects.link(obj)
    trs = layer["placements"]
    for (x,y,z),(_,qy,_,qw),scale in zip(trs["TRANSLATION"],trs["ROTATION"],trs["SCALE"]):
        obj = bpy.data.objects.new("Tuft",None)
        scene.collection.objects.link(obj)
        obj.instance_type = "COLLECTION"
        obj.instance_collection = tuft
        obj.location = (x,-z,y)
        obj.rotation_euler.z = 2*math.atan2(qy,qw)
        obj.scale = scale
        obj.visible_camera = False

bpy.ops.mesh.primitive_plane_add(size=100)
soil = bpy.context.object
mat = bpy.data.materials.new("Soil sky visibility")
mat.use_nodes = True
soil.data.materials.append(mat)
nodes = mat.node_tree.nodes
ao = nodes.new("ShaderNodeAmbientOcclusion")
ao.samples = 64
ao.inputs["Distance"].default_value = 0.32
emission = nodes.new("ShaderNodeEmission")
mat.node_tree.links.new(ao.outputs["AO"],emission.inputs[0])
mat.node_tree.links.new(emission.outputs[0],nodes.get("Material Output").inputs[0])

# X from -11 to 7, glTF Z from -2 to 7. One centimetre per pixel.
camera = placements.get("camera", {"x":-2,"z":2.5,"width":18,"resolution":[1800,900]})
bpy.ops.object.camera_add(location=(camera["x"],-camera["z"],3),rotation=(0,0,0))
scene.camera = bpy.context.object
scene.camera.data.type = "ORTHO"
scene.camera.data.ortho_scale = camera["width"]
scene.render.resolution_x,scene.render.resolution_y = camera["resolution"]
scene.render.resolution_percentage = 100
scene.view_settings.view_transform = "Raw"
scene.render.image_settings.file_format = "PNG"
scene.render.image_settings.color_mode = "BW"
scene.render.dither_intensity = 0
scene.render.filepath = str(output)
bpy.ops.render.render(write_still=True)
