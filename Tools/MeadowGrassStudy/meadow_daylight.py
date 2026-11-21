"""Daylight and first-bounce oak lighting controls for the four-oak meadow."""
import argparse
from pathlib import Path
import shutil

import meadow_aa as aa

OUT = aa.OUT.parent / "meadow-daylight"
EXE = OUT / "MeadowDaylight.exe"
HERE = Path(__file__).resolve().parent / "daylight"
ARMS = ("original", "daylight", "bounce")
POSES = (*aa.oak.distant.tuft.POSES, "bough")


def prepare():
    if not aa.EXE.exists():
        aa.prepare()
    source = OUT / "source"
    if not source.exists():
        shutil.copytree(aa.OUT / "source", source)
    aa.oak.crown.copy_changed(HERE / "Daylight.h", source / "shaders/Daylight.h")
    for file in (aa.OUT / "assets").rglob("*"):
        if file.is_file():
            target = OUT / file.relative_to(aa.OUT)
            target.parent.mkdir(parents=True, exist_ok=True)
            aa.oak.crown.copy_changed(file, target)
    aa.oak.distant.tuft.cards.far.grass.lighting.prepare_viewer(
        upstream=aa.OUT / "source", out=OUT, patch=HERE / "daylight.patch", destination=EXE)


def render(mode, pose, arm, aa_mode="split", **options):
    aa.oak.distant.tuft.cards.GROVE.CAMERAS["bough"] = ((-23.4, -14.8), (-18.6, -18.5), -48.0)
    aa.render(mode, pose, aa_mode, viewer=aa.EXE if arm == "original" else EXE,
              environment={"MEADOW_OAK_BOUNCE": "1" if arm == "bounce" else "0"},
              directory=OUT / "review" / aa_mode / arm, **options)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("mode", choices=("prepare", "view", "capture"))
    parser.add_argument("--pose", choices=POSES, default="canopy")
    parser.add_argument("--aa", choices=aa.MODES, default="split")
    parser.add_argument("--lighting", choices=ARMS, default="bounce")
    args = parser.parse_args()
    if args.mode == "prepare":
        prepare()
    else:
        render(args.mode, args.pose, args.lighting, args.aa)
