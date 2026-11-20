"""Build and capture the three-leaf lighting fixture with the stock viewer."""

import argparse
import hashlib
import json
import math
from pathlib import Path
import struct
import subprocess

from PIL import Image, ImageChops, ImageDraw, ImageFont, PngImagePlugin


ROOT = Path(__file__).resolve().parents[1]
LEAF = ROOT / "assets/source/white-oak-leaf/white-oak-leaf.glb"
LAYERS = (
    ("top", (-0.086, 0.688, 0.006), (8, -7, 11)),
    ("middle", (0.0, 0.610, -0.016), (-7, 9, -9)),
    ("bottom", (0.085, 0.531, 0.013), (5, -10, 8)),
)
VIEWS = (("above", 4, 73), ("side", 4, 2), ("below", 4, -73))
WIDTH, HEIGHT = 3840, 2560
DISTANCE = 0.50
SUN_AZIMUTH, SUN_ELEVATION = -90, 49
TRANSMISSION = 0.45  # Current scalar used by Forked White Oak.


def add(a, b):
    return tuple(x + y for x, y in zip(a, b))


def sub(a, b):
    return tuple(x - y for x, y in zip(a, b))


def dot(a, b):
    return sum(x * y for x, y in zip(a, b))


def rotation(angles):
    pitch, yaw, roll = map(math.radians, angles)
    a, b, c = math.cos(pitch), math.cos(yaw), math.cos(roll)
    d, e, f = math.sin(pitch), math.sin(yaw), math.sin(roll)
    # Column-major glTF transform for Rz(roll) * Ry(yaw) * Rx(pitch).
    return [(c * b, f * b, -e),
            (c * e * d - f * a, f * e * d + c * a, b * d),
            (c * e * a + f * d, f * e * a - c * d, b * a)]


def transform(columns, point):
    return tuple(sum(column[i] * value for column, value in zip(columns, point)) for i in range(3))


def accessor(document, binary, rows, code, type_name, component, target):
    while len(binary) % 4:
        binary.append(0)
    payload = b"".join(struct.pack("<" + code, *row) for row in rows)
    view_index = len(document["bufferViews"])
    document["bufferViews"].append(dict(buffer=0, byteOffset=len(binary), byteLength=len(payload), target=target))
    binary.extend(payload)
    data = dict(bufferView=view_index, componentType=component, count=len(rows), type=type_name)
    if type_name == "VEC3" and target == 34962:
        data.update(min=list(map(min, zip(*rows))), max=list(map(max, zip(*rows))))
    index = len(document["accessors"])
    document["accessors"].append(data)
    return index


def add_twig(document, binary):
    # An opaque, tapered 9 cm crosspiece above the lower leaf's far half.
    start = (0.042, 0.556, -0.052)
    end = (0.134, 0.552, -0.025)
    length = math.hypot(end[0] - start[0], end[2] - start[2])
    cross_x, cross_z = -(end[2] - start[2]) / length, (end[0] - start[0]) / length
    positions, normals, faces = [], [], []
    sides = 12
    for center, radius in ((start, 0.0040), (end, 0.0024)):
        for side in range(sides):
            angle = math.tau * side / sides
            sine = math.sin(angle)
            n = (cross_x * sine, math.cos(angle), cross_z * sine)
            positions.append(add(center, tuple(radius * v for v in n)))
            normals.append(n)
    for i in range(sides):
        j = (i + 1) % sides
        faces.extend(((i, sides + j, sides + i), (i, j, sides + j)))
    for i in range(1, sides - 1):
        faces.extend(((0, i + 1, i), (sides, sides + i, sides + i + 1)))
    document["materials"].append({"name": "Opaque twig control", "doubleSided": True,
                                  "pbrMetallicRoughness": {"baseColorFactor": [0.26, 0.16, 0.08, 1],
                                                          "metallicFactor": 0}})
    twig = {"attributes": {"POSITION": accessor(document, binary, positions, "3f", "VEC3", 5126, 34962),
                           "NORMAL": accessor(document, binary, normals, "3f", "VEC3", 5126, 34962)},
            "indices": accessor(document, binary, [(i,) for face in faces for i in face], "H", "SCALAR", 5123, 34963),
            "material": len(document["materials"]) - 1}
    mesh = len(document["meshes"])
    document["meshes"].append({"name": "Opaque twig control", "primitives": [twig]})
    document["nodes"].append({"name": "Opaque twig control", "mesh": mesh})
    return positions


def make_scene(path):
    data = LEAF.read_bytes()
    json_length = struct.unpack_from("<I", data, 12)[0]
    document = json.loads(data[20:20 + json_length])
    binary = bytearray(data[28 + json_length:])
    primitive, = document["meshes"][0]["primitives"]
    material = document["materials"][primitive["material"]]
    document["extensionsUsed"].append("KHR_materials_diffuse_transmission")
    material.setdefault("extensions", {})["KHR_materials_diffuse_transmission"] = {
        "diffuseTransmissionFactor": TRANSMISSION,
    }
    pos = document["accessors"][primitive["attributes"]["POSITION"]]
    view = document["bufferViews"][pos["bufferView"]]
    offset = view.get("byteOffset", 0) + pos.get("byteOffset", 0)
    stride = view.get("byteStride", 12)
    points = [struct.unpack_from("<3f", binary, offset + i * stride) for i in range(pos["count"])]
    pivot = tuple((a + b) * 0.5 for a, b in zip(pos["min"], pos["max"]))
    world_points = {}
    document["nodes"] = []
    for layer, center, angles in LAYERS:
        columns = rotation(angles)
        translate = sub(center, transform(columns, pivot))
        matrix = [value for column in columns for value in (*column, 0)] + [*translate, 1]
        document["nodes"].append({"name": f"{layer.title()} leaf", "mesh": 0, "matrix": matrix})
        world_points[layer] = [add(transform(columns, p), translate) for p in points]
    all_points = [p for points in world_points.values() for p in points] + add_twig(document, binary)
    focus = tuple((min(axis) + max(axis)) * 0.5 for axis in zip(*all_points))

    document["scenes"] = [{"name": "White oak three-layer lighting fixture", "nodes": list(range(4))}]
    document["scene"] = 0
    document["buffers"][0]["byteLength"] = len(binary)
    metadata = json.dumps(document, separators=(",", ":")).encode("utf-8")
    metadata += b" " * (-len(metadata) % 4)
    binary.extend(b"\0" * (-len(binary) % 4))
    header = struct.pack("<4sII", b"glTF", 2, 28 + len(metadata) + len(binary))
    path.write_bytes(header + struct.pack("<I4s", len(metadata), b"JSON") + metadata
                     + struct.pack("<I4s", len(binary), b"BIN\0") + binary)
    return world_points, focus


def crop_rect(points, focus, yaw, pitch):
    yaw, pitch = map(math.radians, (yaw, pitch))
    right = (math.cos(yaw), 0, -math.sin(yaw))
    up = (-math.sin(yaw) * math.sin(pitch), math.cos(pitch), -math.cos(yaw) * math.sin(pitch))
    back = (math.sin(yaw) * math.cos(pitch), math.sin(pitch), math.cos(yaw) * math.cos(pitch))
    pixels = []
    for point in points:
        delta = sub(point, focus)
        focal = (HEIGHT / 2) / math.tan(math.pi / 8) / (DISTANCE - dot(delta, back))
        pixels.append((WIDTH / 2 + focal * dot(delta, right), HEIGHT / 2 - focal * dot(delta, up)))
    x, y = zip(*pixels)
    return (max(0, math.floor(min(x)) - 38), max(0, math.floor(min(y)) - 38),
            min(WIDTH, math.ceil(max(x)) + 38), min(HEIGHT, math.ceil(max(y)) + 38))


def capture(executable, scene, path, yaw, pitch, *extra):
    subprocess.run([str(executable), "--capture", str(path), "--scene", str(scene),
                    "--width", str(WIDTH), "--height", str(HEIGHT), "--msaa", "4",
                    "--yaw-degrees", str(yaw), "--pitch-degrees", str(pitch), "--distance", str(DISTANCE),
                    "--sun-azimuth", str(SUN_AZIMUTH), "--sun-elevation", str(SUN_ELEVATION),
                    "--wind-strength", "0", "--wind-time", "0", *extra], check=True)


def save_display_png(image, path):
    metadata = PngImagePlugin.PngInfo()
    metadata.add(b"sRGB", b"\x00")
    image.save(path, pnginfo=metadata)


def make_sheet(directory):
    column, row = 500, 412
    sheet = Image.new("RGB", (column * 3 + 48, row * 3 + 93), "#20252a")
    draw = ImageDraw.Draw(sheet)
    title = ImageFont.truetype("segoeui.ttf", 28)
    label = ImageFont.truetype("segoeui.ttf", 22)
    draw.text((24, 16), "White oak leaf stack  /  Current renderer", font=title, fill="#f4f6f7")
    for j, (layer, _, _) in enumerate(LAYERS):
        for i, (view, _, _) in enumerate(VIEWS):
            x, y = 24 + i * column, 64 + j * row
            draw.text((x, y), f"{layer.title()}  /  {view.title()}", font=label, fill="#f4f6f7")
            with Image.open(directory / f"{layer}-{view}.png") as crop:
                crop.thumbnail((column - 18, row - 46), Image.Resampling.LANCZOS)
                sheet.paste(crop, (x + (column - 18 - crop.width) // 2, y + 36))
    save_display_png(sheet, directory / "leaf-stack-comparison.png")


def compare_shadows(directory, crops):
    with (Image.open(directory / "stack-above.png") as shadowed,
          Image.open(directory / "stack-above-no-leaf-shadow-reception.png") as unshadowed):
        difference = ImageChops.difference(shadowed.convert("RGB"), unshadowed.convert("RGB"))
        if not difference.getbbox():
            raise RuntimeError("Disabling leaf shadow reception did not change the overhead capture.")
        left, top, right, bottom = zip(*crops.values())
        region = (min(left), min(top), max(right), max(bottom))
        sheet = Image.new("RGB", (1440, 722), "#20252a")
        draw = ImageDraw.Draw(sheet)
        label = ImageFont.truetype("segoeui.ttf", 24)
        for i, (source, title) in enumerate(((shadowed, "Leaf shadow reception on"),
                                             (unshadowed, "Leaf shadow reception off"))):
            x = 24 + 708 * i
            draw.text((x, 18), title, font=label, fill="#f4f6f7")
            panel = source.crop(region)
            panel.thumbnail((684, 650), Image.Resampling.LANCZOS)
            sheet.paste(panel, (x, 58))
        save_display_png(sheet, directory / "leaf-shadow-reception-comparison.png")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("output", type=Path, help="New directory for the GLB and captures")
    parser.add_argument("--engine", type=Path, default=ROOT / "build/Release/CustomGameEngine.exe")
    args = parser.parse_args()

    output = args.output.resolve()
    output.mkdir(parents=True)
    scene = output / "WhiteOak-LeafLightingFixture.glb"
    points, focus = make_scene(scene)
    engine = args.engine.resolve()
    views = {}
    for view, yaw, pitch in VIEWS:
        frame = output / f"stack-{view}.png"
        capture(engine, scene, frame, yaw, pitch)
        crops = {layer: crop_rect(vertices, focus, yaw, pitch) for layer, vertices in points.items()}
        views[view] = {"yaw_degrees": yaw, "pitch_degrees": pitch, "crop_pixels": crops}
        with Image.open(frame) as image:
            for layer, rect in crops.items():
                save_display_png(image.crop(rect), output / f"{layer}-{view}.png")
    capture(engine, scene, output / "stack-above-no-leaf-shadow-reception.png",
            *VIEWS[0][1:], "--no-leaf-shadow-reception")
    make_sheet(output)
    compare_shadows(output, views["above"]["crop_pixels"])
    manifest = {"engine_sha256": hashlib.sha256(engine.read_bytes()).hexdigest(),
                "scene_sha256": hashlib.sha256(scene.read_bytes()).hexdigest(),
                "frame_size": (WIDTH, HEIGHT), "msaa": 4, "wind_strength": 0, "wind_time": 0,
                "camera_focus_m": focus, "camera_distance_m": DISTANCE,
                "sun_azimuth_degrees": SUN_AZIMUTH, "sun_elevation_degrees": SUN_ELEVATION,
                "leaf_transmission": TRANSMISSION, "views": views}
    (output / "capture-settings.json").write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")


if __name__ == "__main__":
    main()
