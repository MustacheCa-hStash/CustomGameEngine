"""Render the matched leaf response in Blender Cycles with direct lighting only.

blender -b -t 12 --python Tools/RenderLeafLightingReference.py -- <review directory> <sky.hdr> <output>
"""

import argparse
import json
import math
from pathlib import Path
import struct
import sys
import tempfile

import bpy
from mathutils import Vector
import numpy as np


def to_blender(x, y, z):
    return Vector((x, -z, y))


def make_linear_image(name, pixels):
    height, width, _ = pixels.shape
    image = bpy.data.images.new(name, width, height, float_buffer=True)
    image.colorspace_settings.name = "Linear Rec.709"
    image.pixels.foreach_set(np.ravel(pixels))
    image.update()
    return image


def make_world(scene, dome_path):
    dome = bpy.data.images.load(str(dome_path))
    width, height = dome.size
    pixels = np.empty((height, width, 4), dtype=np.float32)
    dome.pixels.foreach_get(pixels.ravel())
    # Cycles maps longitude from +X with the opposite winding. Convert from the engine's +Z axis.
    columns = (width * 5 // 4 - 1 - np.arange(width)) % width
    backdrop = pixels[:, columns, :].copy()
    radiance = backdrop.copy()
    radiance[:height // 2, :, :3] = 0
    # Integrate the exact upper hemisphere and match the engine's neutral +up calibration.
    elevation = np.linspace(0, math.pi, height + 1) - math.pi / 2
    y = np.sin(elevation)
    cosine_weights = (np.maximum(y[1:], 0) ** 2 - np.maximum(y[:-1], 0) ** 2) / width
    luminance = radiance[:, :, :3] @ np.array([0.2126, 0.7152, 0.0722], dtype=np.float32)
    gain = 0.35 / np.sum(luminance * cosine_weights[:, None])
    radiance[:, :, :3] *= gain

    world = bpy.data.worlds.new("Meadow Day")
    scene.world = world
    world.use_nodes = True
    nodes, links = world.node_tree.nodes, world.node_tree.links
    nodes.clear()
    lighting = nodes.new("ShaderNodeBackground")
    environment = nodes.new("ShaderNodeTexEnvironment")
    environment.image = make_linear_image("Meadow Day upper-hemisphere radiance", radiance)
    visible = nodes.new("ShaderNodeBackground")
    sky = nodes.new("ShaderNodeTexEnvironment")
    sky.image = make_linear_image("Meadow Day backdrop", backdrop)
    path = nodes.new("ShaderNodeLightPath")
    mix = nodes.new("ShaderNodeMixShader")
    output = nodes.new("ShaderNodeOutputWorld")
    links.new(environment.outputs["Color"], lighting.inputs["Color"])
    links.new(sky.outputs["Color"], visible.inputs["Color"])
    links.new(path.outputs["Is Camera Ray"], mix.inputs[0])
    links.new(lighting.outputs[0], mix.inputs[1])
    links.new(visible.outputs[0], mix.inputs[2])
    links.new(mix.outputs[0], output.inputs["Surface"])
    return lighting


def diffuse_material(material, color, reflectance_texture=None, transmission_texture=None, fraction=0):
    material.use_nodes = True
    nodes, links = material.node_tree.nodes, material.node_tree.links
    nodes.clear()
    output = nodes.new("ShaderNodeOutputMaterial")
    diffuse = nodes.new("ShaderNodeBsdfDiffuse")
    diffuse.inputs["Color"].default_value = (*color, 1)
    if reflectance_texture:
        image = nodes.new("ShaderNodeTexImage")
        image.image = reflectance_texture
        links.new(image.outputs["Color"], diffuse.inputs["Color"])
    if transmission_texture:
        image = nodes.new("ShaderNodeTexImage")
        image.image = transmission_texture
        translucent = nodes.new("ShaderNodeBsdfTranslucent")
        links.new(image.outputs["Color"], translucent.inputs["Color"])
        mix = nodes.new("ShaderNodeMixShader")
        mix.inputs[0].default_value = fraction
        links.new(diffuse.outputs[0], mix.inputs[1])
        links.new(translucent.outputs[0], mix.inputs[2])
        links.new(mix.outputs[0], output.inputs["Surface"])
    else:
        links.new(diffuse.outputs[0], output.inputs["Surface"])


def select_device(scene):
    prefs = bpy.context.preferences.addons["cycles"].preferences
    for backend in ("OPTIX", "CUDA"):
        try:
            prefs.compute_device_type = backend
            prefs.refresh_devices()
        except (RuntimeError, TypeError):
            continue
        if any(device.type == backend for device in prefs.devices):
            for device in prefs.devices:
                device.use = device.type == backend
            scene.cycles.device = "GPU"
            return backend
    return "CPU"


def load_transmission_map(fixture):
    data = fixture.read_bytes()
    metadata_size = struct.unpack_from("<I", data, 12)[0]
    document = json.loads(data[20:20 + metadata_size])
    image = next(image for image in document["images"] if image["name"] == "White oak leaf transmission")
    view = document["bufferViews"][image["bufferView"]]
    offset = 28 + metadata_size + view.get("byteOffset", 0)
    with tempfile.TemporaryDirectory() as directory:
        path = Path(directory) / "White oak leaf transmission.png"
        path.write_bytes(data[offset:offset + view["byteLength"]])
        texture = bpy.data.images.load(str(path))
        texture.pack()
    return texture


def prepare_scene(fixture, manifest, dome, samples):
    bpy.ops.wm.read_factory_settings(use_empty=True)
    bpy.ops.import_scene.gltf(filepath=str(fixture))
    scene = bpy.context.scene
    scene.render.engine = "CYCLES"
    scene.cycles.samples = samples
    scene.cycles.use_adaptive_sampling = True
    scene.cycles.adaptive_threshold = 0.025
    scene.cycles.use_denoising = False
    # Evaluate direct light at the first surface, while excluding every indirect bounce.
    scene.cycles.max_bounces = 0
    scene.cycles.use_light_tree = False
    scene.render.resolution_x, scene.render.resolution_y = manifest["frame_size"]
    scene.render.resolution_percentage = 100
    scene.render.image_settings.file_format = "OPEN_EXR"
    scene.render.image_settings.color_mode = "RGB"
    scene.render.image_settings.color_depth = "16"
    scene.view_settings.view_transform = "Standard"
    scene.view_settings.exposure = manifest["exposure_ev"]
    scene.view_settings.gamma = 1

    leaf = bpy.data.materials["White oak | leaf surface"]
    twig = bpy.data.materials["Opaque twig control"]
    base = bpy.data.images["white-oak-leaf-base-color"]
    transmitted = load_transmission_map(fixture)
    diffuse_material(leaf, (1, 1, 1), base, transmitted, manifest["leaf_transmission"])
    diffuse_material(twig, (0.26, 0.16, 0.08))
    ground = bpy.data.materials.new("Ground")
    diffuse_material(ground, (0.30, 0.36, 0.30))
    bpy.ops.mesh.primitive_plane_add(size=8)
    bpy.context.object.data.materials.append(ground)
    bpy.context.object.visible_shadow = False

    light_data = bpy.data.lights.new("Sun", "SUN")
    light_data.angle = 0
    sun = bpy.data.objects.new("Sun", light_data)
    scene.collection.objects.link(sun)
    azimuth = math.radians(manifest["sun_azimuth_degrees"])
    elevation = math.radians(manifest["sun_elevation_degrees"])
    to_sun = to_blender(math.sin(azimuth) * math.cos(elevation), math.sin(elevation),
                        math.cos(azimuth) * math.cos(elevation))
    sun.rotation_euler = (-to_sun).to_track_quat("-Z", "Y").to_euler()
    sky = make_world(scene, dome)

    camera_data = bpy.data.cameras.new("Matched orbit")
    camera_data.sensor_fit = "VERTICAL"
    camera_data.lens = camera_data.sensor_height / (2 * math.tan(math.pi / 8))
    camera_data.clip_start = 0.01
    camera = bpy.data.objects.new("Matched orbit", camera_data)
    scene.collection.objects.link(camera)
    scene.camera = camera
    print("Cycles device:", select_device(scene), flush=True)
    return scene, camera, light_data, sky


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("review", type=Path)
    parser.add_argument("sky", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("--samples", type=int, default=32)
    args = parser.parse_args(sys.argv[sys.argv.index("--") + 1:])
    manifest = json.loads((args.review / "capture-settings.json").read_text())
    fixture = args.review / "WhiteOak-LeafResponseFixture.glb"
    args.output.mkdir(parents=True, exist_ok=False)
    scene, camera, sun, sky = prepare_scene(fixture.resolve(), manifest, args.sky.resolve(), args.samples)
    focus = to_blender(*manifest["camera_focus_m"])
    distance = manifest["camera_distance_m"]
    for view, pose in manifest["views"].items():
        yaw = math.radians(pose["yaw_degrees"])
        pitch = math.radians(pose["pitch_degrees"])
        back = to_blender(math.sin(yaw) * math.cos(pitch), math.sin(pitch),
                          math.cos(yaw) * math.cos(pitch))
        camera.location = focus + distance * back
        camera.rotation_euler = (-back).to_track_quat("-Z", "Y").to_euler()
        for name, direct, diffuse in (("sun-only", True, False), ("sky-only", False, True), ("combined", True, True)):
            sun.energy = math.pi * 0.65 if direct else 0
            sky.inputs["Strength"].default_value = 1 if diffuse else 0
            scene.render.filepath = str((args.output / f"{view}-{name}.exr").resolve())
            bpy.ops.render.render(write_still=True)


if __name__ == "__main__":
    main()
