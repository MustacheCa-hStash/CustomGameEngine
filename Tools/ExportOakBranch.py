"""Run inside Blender with the low-branched oak or its single-branch source open."""

import json
from pathlib import Path
import struct
import sys

import bpy


output = Path(sys.argv[sys.argv.index("--") + 1]).resolve()
full_tree = "Branch and trunk geometry" in bpy.data.objects
bark = bpy.data.objects["Branch and trunk geometry" if full_tree else "Oak branch - bark"]
leaves = bpy.data.objects["Individual curved leaf geometry" if full_tree else "Oak branch - modeled leaves"]

bpy.ops.object.select_all(action="DESELECT")
minimum_z = 0.0 if full_tree else min((bark.matrix_world @ vertex.co).z for vertex in bark.data.vertices)
for obj in (bark, leaves):
    obj.location.z -= minimum_z
    obj.select_set(True)

# Blender's procedural leaf shader is represented by a solid color in this export.
material = leaves.data.materials[0]
shader = material.node_tree.nodes.get("Principled BSDF")
base_color = shader.inputs["Base Color"]
for link in list(base_color.links):
    material.node_tree.links.remove(link)
base_color.default_value = (0.060, 0.115, 0.024, 1.0)
material.use_backface_culling = False
# Use a flat texture so the viewer uses its linear-to-display texture shading path.
image = bpy.data.images.new("Oak leaf base color", width=4, height=4)
image.colorspace_settings.name = "sRGB"
image.pixels = [0.272, 0.373, 0.168, 1.0] * 16
image.pack()
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
