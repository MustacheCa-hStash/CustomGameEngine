"""Shaped meadow growth and camera-only grass profiling in the four-oak scene."""
import argparse
import json
import subprocess
import sys

import numpy as np
from PIL import Image

import far_shadow as far

OUT = far.OUT.parent / "meadow-camera"
STAGE = OUT / "scene"
VIEWER = STAGE / "CameraMeadow.exe"
REVIEW = OUT / "review"
LAYERS = {"all": None, "short": "MEADOW_HIDE_TALL_CAMERA", "bare": "MEADOW_HIDE_ALL_GRASS_CAMERA"}
SIZES = {"1440p": (2560, 1440), "4k": (3840, 2160)}


def stage():
    assets = STAGE / "assets"
    assets.mkdir(parents=True, exist_ok=True)
    for source in (far.STAGE / "assets").iterdir():
        if source.name != "MeadowCover.png":
            far.grass.copy_file(source, assets / source.name)
    far.grass.copy_file(far.STAGE / "scene.json", STAGE / "scene.json")
    with Image.open(far.STAGE / "assets/MeadowCover.png") as source:
        cover = np.array(source.convert("RGB"))
    height, width = cover.shape[:2]
    x, z = np.meshgrid((np.arange(width)+.5)*80/width-48, (np.arange(height)+.5)*80/height-48)
    # An elongated stand on the west shoulder, with scalloped margins and uneven growth.
    spine = -25 - .2*(z+12) + 1.3*np.sin(.21*(z+14))
    breadth = 4.9 + .8*np.sin(.64*z) + .55*np.cos(.35*z)
    radius = np.hypot((x-spine)/breadth, (z+13)/15)
    edge = .065*np.sin(1.7*x+.7*z) + .04*np.cos(2.2*z-.5*x)
    growth = np.clip((1.13 + edge - radius)/.55, 0, 1)
    growth = growth**2*(3-2*growth) * (.86+.09*np.sin(.53*x+.29*z)+.05*np.cos(1.2*z))
    cover[:,:,2] = np.uint8(255*growth + .5)
    Image.fromarray(cover).save(assets / "MeadowCover.png")


def prepare():
    if not (far.STAGE / "assets/MeadowCover.png").exists():
        subprocess.run([sys.executable, "-B", far.__file__, "prepare"], check=True)
    stage()
    far.grass.lighting.prepare_viewer(upstream=far.OUT/"source", out=OUT,
                                      patch=far.grass.HERE/"camera_meadow.patch", destination=VIEWER)


def capture(pose, layer="all", size="4k", visual=True, mode="capture", out=None, counters=False):
    env = far.environment("both")
    for flag in ("MEADOW_BASE_COLOUR", *filter(None, LAYERS.values())):
        env.pop(flag, None)
    if not visual:
        env["MEADOW_BASE_COLOUR"] = "1"
    if LAYERS[layer]:
        env[LAYERS[layer]] = "1"
    w,h = SIZES[size]
    options = ["--width", str(w), "--height", str(h)]
    if counters:
        options.append("--camera-pipeline-stats")
    destination = out or REVIEW/("bank" if visual else "original")
    if out is None and (size != "4k" or layer != "all"):
        destination = destination/size/layer
    far.grove.capture(pose, mode, "combined", False, viewer=VIEWER, extra_args=options,
                      focus=json.loads((STAGE/"scene.json").read_text())["focus"],
                      output_dir=destination, environment=env)


def benchmark():
    for pose in far.CAMERAS:
        capture(pose, visual=False)
    cases = [(pose, size, layer) for pose in far.CAMERAS for size in SIZES for layer in LAYERS]
    for run, order in (("forward", cases), ("reverse", reversed(cases))):
        for pose, size, layer in order:
            print(run, pose, size, layer, flush=True)
            capture(pose, layer, size, out=REVIEW/run/size/layer)
    for layer in LAYERS:
        capture("downhill", layer, out=REVIEW/"counters"/layer, counters=True)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("mode", choices=("prepare", "capture", "view", "benchmark"))
    parser.add_argument("--pose", choices=far.CAMERAS, default="downhill")
    parser.add_argument("--arm", choices=("bank", "original"), default="bank")
    parser.add_argument("--layer", choices=LAYERS, default="all")
    parser.add_argument("--size", choices=SIZES, default="4k")
    args = parser.parse_args()
    if args.mode == "prepare":
        prepare()
    elif args.mode == "benchmark":
        benchmark()
    else:
        capture(args.pose, args.layer, args.size, args.arm == "bank", args.mode)
