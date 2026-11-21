"""Use the remodeled oak's qualified bake in the four-oak distant meadow."""
import argparse
from pathlib import Path
import json

import distant
import optimize_crown as crown

OUT = distant.OUT.parent / "meadow-adaptive"
EXE = OUT / "AdaptiveMeadow.exe"
HERE = Path(__file__).resolve().parent


def prepare():
    if not distant.EXE.is_file():
        import subprocess
        import sys
        subprocess.run([sys.executable, HERE / "distant.py", "prepare"], check=True)
    crown.prepare()
    assets = OUT / "assets"
    oak = assets / "oak"
    oak.mkdir(parents=True, exist_ok=True)
    for file in (distant.OUT / "assets").iterdir():
        if file.is_file():
            crown.copy_changed(file, assets / file.name)
    manifest = crown.ASSET.with_suffix(".spraylods")
    for name in [crown.ASSET.name, manifest.name, *manifest.read_text().split()]:
        crown.copy_changed(crown.ASSET.parent / name, oak / name)
    distant.tuft.cards.far.grass.lighting.prepare_viewer(
        upstream=distant.OUT / "source", out=OUT, patch=HERE / "adaptive_oaks.patch", destination=EXE)


def render(pose, mode, *, adaptive=True, lighting="combined", run="single", samples=0, flags=None):
    grove = distant.tuft.cards.GROVE
    grove.CAMERAS["foreground"] = ((-26., -13.), (-30., -10.), 32.)
    env = distant.environment("optimized")
    env.pop("MEADOW_ADAPTIVE_OAK", None)
    options = []
    if adaptive:
        env["MEADOW_ADAPTIVE_OAK"] = "1"
        options.extend([*crown.GPU_FLAGS, crown.REFERENCE_GUARD])
    if flags:
        env.update(flags)
    if mode == "capture" and samples:
        options.extend(["--timing-samples", str(samples)])
    grove.capture(pose, mode, lighting, False, viewer=EXE, environment=env, extra_args=options,
                  focus=json.loads((distant.tuft.cards.STAGE / "scene.json").read_text())["focus"],
                  output_dir=OUT / "review" / run / ("adaptive" if adaptive else "fixed"))


if __name__ == "__main__":
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("mode", choices=("prepare", "capture", "view"))
    ap.add_argument("--pose", choices=distant.tuft.POSES, default="canopy")
    args = ap.parse_args()
    if args.mode == "prepare":
        prepare()
    else:
        render(args.pose, args.mode)
