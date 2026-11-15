"""Run inside Blender with the low-branched oak or its single-branch source open."""

import json
from pathlib import Path
from statistics import median
import struct
import sys

import bpy


def bake_leaf_base_color(leaves, material):
    # Every leaf uses the full UV square. Bake the authored color graph once on a
    # quad, with a representative LeafVariation value from the source mesh.
    points = [(0, 0, 0), (1, 0, 0), (1, 1, 0), (0, 1, 0)]
    mesh = bpy.data.meshes.new("Leaf base-color bake mesh")
    mesh.from_pydata(points, [], [(0, 1, 2, 3)])
    uv = mesh.uv_layers.new(name="UVMap")
    for loop in mesh.loops:
        uv.data[loop.index].uv = points[loop.vertex_index][:2]
    variation = leaves.data.attributes.get("LeafVariation")
    if variation is not None:
        tone = median(item.value for item in variation.data)
        mesh.attributes.new("LeafVariation", "FLOAT", "POINT").data.foreach_set("value", [tone] * len(points))

    bake_object = bpy.data.objects.new("Leaf base-color bake", mesh)
    bpy.context.scene.collection.objects.link(bake_object)
    bake_material = material.copy()
    mesh.materials.append(bake_material)
    nodes, links = bake_material.node_tree.nodes, bake_material.node_tree.links
    color = nodes.get("Principled BSDF").inputs["Base Color"]
    emit = nodes.new("ShaderNodeEmission")
    links.new(color.links[0].from_socket, emit.inputs["Color"])
    links.new(emit.outputs[0], nodes.get("Material Output").inputs["Surface"])

    image = bpy.data.images.new("Oak leaf base color", width=512, height=512, alpha=False)
    image.colorspace_settings.name = "sRGB"
    image.file_format = "PNG"
    target = nodes.new("ShaderNodeTexImage")
    target.image = image
    nodes.active = target

    scene = bpy.context.scene
    scene.render.engine = "CYCLES"
    scene.cycles.device = "CPU"
    scene.cycles.samples = 8
    bpy.context.view_layer.objects.active = bake_object
    bake_object.select_set(True)
    if bpy.ops.object.bake(type="EMIT", use_selected_to_active=False, margin=0) != {"FINISHED"}:
        raise RuntimeError("Could not bake the modeled leaf base color.")
    image.pack()

    bpy.data.objects.remove(bake_object, do_unlink=True)
    bpy.data.meshes.remove(mesh)
    bpy.data.materials.remove(bake_material)
    return image


output = Path(sys.argv[sys.argv.index("--") + 1]).resolve()
full_tree = "Branch and trunk geometry" in bpy.data.objects
bark = bpy.data.objects["Branch and trunk geometry" if full_tree else "Oak branch - bark"]
leaves = bpy.data.objects["Individual curved leaf geometry" if full_tree else "Oak branch - modeled leaves"]

bpy.ops.object.select_all(action="DESELECT")
material = leaves.data.materials[0]
if full_tree:
    image = bpy.data.images.new("Oak leaf base color", width=4, height=4)
    image.colorspace_settings.name = "sRGB"
    image.pixels = [0.272, 0.373, 0.168, 1.0] * 16
    image.pack()
else:
    image = bake_leaf_base_color(leaves, material)

minimum_z = 0.0 if full_tree else min((bark.matrix_world @ vertex.co).z for vertex in bark.data.vertices)
for obj in (bark, leaves):
    obj.location.z -= minimum_z
    obj.select_set(True)

shader = material.node_tree.nodes.get("Principled BSDF")
base_color = shader.inputs["Base Color"]
for link in list(base_color.links):
    material.node_tree.links.remove(link)
base_color.default_value = (0.060, 0.115, 0.024, 1.0)
material.use_backface_culling = False
color_node = material.node_tree.nodes.new("ShaderNodeTexImage")
color_node.image = image
material.node_tree.links.new(color_node.outputs["Color"], base_color)

bpy.ops.export_scene.gltf(filepath=str(output), export_format="GLB",
                          use_selection=True, export_yup=True)

# Add the thin-surface signal consumed by the viewer. Preserve all binary buffers.
glb = output.read_bytes()
json_length, chunk_type = struct.unpack_from("<I4s", glb, 12)
assert chunk_type == b"JSON"
document = json.loads(glb[20:20 + json_length])
leaf_material = next(item for item in document["materials"] if item["name"] == material.name)
extension = "KHR_materials_diffuse_transmission"
leaf_material.setdefault("extensions", {})[extension] = {"diffuseTransmissionFactor": 0.25}
used = document.setdefault("extensionsUsed", [])
if extension not in used:
    used.append(extension)
encoded = json.dumps(document, ensure_ascii=False, separators=(",", ":")).encode("utf-8")
encoded += b" " * (-len(encoded) % 4)
tail = glb[20 + json_length:]
output.write_bytes(struct.pack("<4sIII4s", b"glTF", 2, 20 + len(encoded) + len(tail),
                               len(encoded), b"JSON") + encoded + tail)
