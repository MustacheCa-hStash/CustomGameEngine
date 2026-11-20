"""Recapture the approved leaf fixture for a lighting review."""

import argparse
import hashlib
import io
import json
from pathlib import Path
import struct
import subprocess

from PIL import Image, ImageDraw, ImageFont

from CaptureLeafLightingBaseline import ROOT, save_display_png


LAYERS = ("top", "middle", "bottom")
VIEWS = ("above", "side", "below")
BASELINE_PANEL_SIZE = (482, 366)
BACKGROUND = "#20252a"
FOREGROUND = "#f4f6f7"


def file_hash(path):
    with path.open("rb") as source:
        return hashlib.file_digest(source, "sha256").hexdigest()


def make_comparison(baseline, output, settings, exposure, stage):
    panel_width, panel_height = BASELINE_PANEL_SIZE
    margin, gap = 24, 12
    cell_width = 2 * panel_width + gap + margin
    row_height = panel_height + 80
    width = 3 * cell_width + margin
    sheet = Image.new("RGB", (width, 105 + 3 * row_height), BACKGROUND)
    draw = ImageDraw.Draw(sheet)
    title = ImageFont.truetype("segoeuib.ttf", 36)
    label = ImageFont.truetype("segoeuib.ttf", 26)
    detail = ImageFont.truetype("segoeui.ttf", 22)
    heading, before_label, after_label = {
        "hdr": ("Fixed-exposure HDR", "Approved baseline", "Linear HDR"),
        "skylight": ("Directional diffuse skylight", "Approved HDR", "Directional skylight"),
        "leaf": ("Two-sided leaf response", "Approved skylight", "Two-sided leaf response"),
    }[stage]
    draw.text((margin, 16), f"White oak leaf stack / {heading}", font=title, fill=FOREGROUND)
    frame_width, frame_height = settings["frame_size"]
    azimuth, elevation = settings["sun_azimuth_degrees"], settings["sun_elevation_degrees"]
    draw.text((margin, 64),
              f"{frame_width} × {frame_height}   /   {settings['msaa']}× MSAA   /   "
              f"Wind {settings['wind_strength']}   /   Sun {azimuth}° azimuth, {elevation}° elevation   /   "
              f"HDR exposure {exposure:+.2f} EV",
              font=detail, fill="#c7d0d6")

    with Image.open(baseline) as before:
        expected_size = (1548, 1329) if stage == "hdr" else sheet.size
        if before.size != expected_size:
            raise ValueError("The baseline sheet does not match the approved nine-view layout.")
        for row, layer in enumerate(LAYERS):
            for column, view in enumerate(VIEWS):
                x, y = margin + column * cell_width, 105 + row * row_height
                after_x = x + panel_width + gap
                draw.text((x, y), f"{layer.title()} / {view.title()}", font=label, fill=FOREGROUND)
                draw.text((x, y + 35), f"Before · {before_label}", font=detail, fill="#c7d0d6")
                draw.text((after_x, y + 35), f"After · {after_label}", font=detail, fill="#c7d0d6")
                baseline_x, baseline_y = (24 + column * 500, 100 + row * 412) if stage == "hdr" else (after_x, y + 64)
                panel = before.crop((baseline_x, baseline_y, baseline_x + panel_width, baseline_y + panel_height))
                sheet.paste(panel, (x, y + 64))
                with Image.open(output / f"{layer}-{view}.png") as after:
                    after.thumbnail(BASELINE_PANEL_SIZE, Image.Resampling.LANCZOS)
                    sheet.paste(after, (after_x + (panel_width - after.width) // 2, y + 64))
    save_display_png(sheet, output / "leaf-stack-before-after.png")


def add_leaf_transmission_map(fixture, output):
    data = fixture.read_bytes()
    metadata_size = struct.unpack_from("<I", data, 12)[0]
    document = json.loads(data[20:20 + metadata_size])
    binary = bytearray(data[28 + metadata_size:])
    material = next(material for material in document["materials"]
                    if "KHR_materials_diffuse_transmission" in material.get("extensions", {}))
    transmission = material["extensions"]["KHR_materials_diffuse_transmission"]

    # The spray bake edge-pads the leaf maps by 256 texels on the top and bottom.
    source = ROOT / "assets/source/white-oak-spray/T_OakLeaf_Transmission.png"
    texture_bytes = io.BytesIO()
    with Image.open(source) as texture:
        if texture.size != (1024, 2048):
            raise ValueError("The authored leaf transmission map does not match the spray UV layout.")
        texture.crop((0, 256, 1024, 1792)).save(texture_bytes, format="PNG")
    payload = texture_bytes.getvalue()
    view = len(document["bufferViews"])
    document["bufferViews"].append({"buffer": 0, "byteOffset": len(binary), "byteLength": len(payload)})
    binary.extend(payload)
    image = len(document["images"])
    document["images"].append({"name": "White oak leaf transmission", "mimeType": "image/png", "bufferView": view})
    texture = len(document["textures"])
    document["textures"].append({"source": image})
    transmission["diffuseTransmissionColorTexture"] = {"index": texture}

    document["buffers"][0]["byteLength"] = len(binary)
    metadata = json.dumps(document, separators=(",", ":")).encode("utf-8")
    metadata += b" " * (-len(metadata) % 4)
    binary.extend(b"\0" * (-len(binary) % 4))
    output.write_bytes(struct.pack("<4sII", b"glTF", 2, 28 + len(metadata) + len(binary))
                       + struct.pack("<I4s", len(metadata), b"JSON") + metadata
                       + struct.pack("<I4s", len(binary), b"BIN\0") + binary)
    return file_hash(source)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("baseline", type=Path, help="Directory with the approved sheet, GLB, and capture-settings.json")
    parser.add_argument("output", type=Path, help="New directory for the lighting review")
    parser.add_argument("--engine", type=Path, default=ROOT / "build/Release/CustomGameEngine.exe")
    parser.add_argument("--exposure-ev", type=float, default=0.0)
    references = parser.add_mutually_exclusive_group()
    references.add_argument("--hdr-baseline", type=Path, help="Use the approved HDR review as the before set")
    references.add_argument("--skylight-baseline", type=Path, help="Use the approved skylight review as the before set")
    args = parser.parse_args()
    if not -8.0 <= args.exposure_ev <= 8.0:
        parser.error("--exposure-ev must be between -8 and 8")

    baseline, output, engine = args.baseline.resolve(), args.output.resolve(), args.engine.resolve()
    fixture = baseline / "WhiteOak-LeafLightingFixture.glb"
    approved_sheet = baseline / "leaf-stack-comparison.png"
    reference = baseline
    stage = "hdr"
    if args.hdr_baseline or args.skylight_baseline:
        reference = (args.hdr_baseline or args.skylight_baseline).resolve()
        approved_sheet = reference / "leaf-stack-before-after.png"
        stage = "leaf" if args.skylight_baseline else "skylight"
    settings = json.loads((reference / "capture-settings.json").read_text(encoding="utf-8"))
    if file_hash(fixture) != settings["scene_sha256"]:
        raise ValueError("The fixture GLB does not match capture-settings.json.")
    baseline_sheet_hash = file_hash(approved_sheet)
    baseline_engine_hash = settings["engine_sha256"]
    engine_hash = file_hash(engine)
    if stage != "hdr":
        if args.exposure_ev != settings["exposure_ev"]:
            parser.error("--exposure-ev must match the approved baseline")
        sky = engine.parent / "assets/MeadowDaySky.hdr"
        sky_hash = file_hash(sky)
        if stage == "leaf" and sky_hash != settings["sky_sha256"]:
            raise ValueError("The sky dome does not match the approved skylight baseline.")
        settings.update(lighting="two-sided leaf response" if stage == "leaf" else "directional diffuse skylight",
                        sky_sha256=sky_hash)
    output.mkdir(parents=True, exist_ok=False)
    if stage == "leaf":
        source = fixture
        fixture = output / "WhiteOak-LeafResponseFixture.glb"
        settings["transmission_map_source_sha256"] = add_leaf_transmission_map(source, fixture)
        settings["baseline_scene_sha256"] = settings["scene_sha256"]
        settings["scene_sha256"] = file_hash(fixture)
    width, height = settings["frame_size"]
    for name in VIEWS:
        view = settings["views"][name]
        plate = output / f"stack-{name}.png"
        subprocess.run([str(engine), "--capture", str(plate), "--scene", str(fixture),
                        "--width", str(width), "--height", str(height), "--msaa", str(settings["msaa"]),
                        "--yaw-degrees", str(view["yaw_degrees"]), "--pitch-degrees", str(view["pitch_degrees"]),
                        "--distance", str(settings["camera_distance_m"]),
                        "--sun-azimuth", str(settings["sun_azimuth_degrees"]),
                        "--sun-elevation", str(settings["sun_elevation_degrees"]),
                        "--wind-strength", str(settings["wind_strength"]), "--wind-time", str(settings["wind_time"]),
                        "--exposure-ev", str(args.exposure_ev),
                        "--linear-hdr", str(output / f"stack-{name}-linear.dds")], check=True)
        with Image.open(plate) as image:
            if image.size != (width, height):
                raise ValueError(f"Unexpected frame size for {name}.")
            for layer in LAYERS:
                save_display_png(image.crop(tuple(view["crop_pixels"][layer])), output / f"{layer}-{name}.png")

    make_comparison(approved_sheet, output, settings, args.exposure_ev, stage)
    settings.update(engine_sha256=engine_hash, baseline_engine_sha256=baseline_engine_hash,
                    baseline_sheet_sha256=baseline_sheet_hash, exposure_ev=args.exposure_ev,
                    linear_plate_format="DDS R16G16B16A16_FLOAT (before exposure and tone mapping)",
                    display_plate_format="sRGB PNG")
    (output / "capture-settings.json").write_text(json.dumps(settings, indent=2) + "\n", encoding="utf-8")


if __name__ == "__main__":
    main()
