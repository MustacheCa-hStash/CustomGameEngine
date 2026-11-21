"""Prepare, capture and interactively compare foliage AA in the adaptive meadow."""
import argparse
import json
from pathlib import Path
import shutil

import adaptive_oaks as oak

OUT = oak.OUT.parent / "meadow-aa"
EXE = OUT / "MeadowAA.exe"
HERE = Path(__file__).resolve().parent / "aa"
MODES = ("off", "msaa", "hdr", "split", "temporal")


def prepare():
    if not oak.EXE.exists():
        oak.prepare()
    source = OUT / "source"
    if not source.exists():
        shutil.copytree(oak.OUT / "source", source)
    for name, subdir in (("MeadowAA.inl","src"), ("MeadowAA.hlsl","shaders")):
        oak.crown.copy_changed(HERE/name,source/subdir/name)
    for file in (oak.OUT / "assets").rglob("*"):
        if file.is_file():
            target = OUT / file.relative_to(oak.OUT)
            target.parent.mkdir(parents=True, exist_ok=True)
            oak.crown.copy_changed(file,target)
    oak.distant.tuft.cards.far.grass.lighting.prepare_viewer(upstream=oak.OUT/"source",
        out=OUT, patch=HERE/"meadow_aa.patch", destination=EXE)


def render(mode, pose, aa, samples=0, extra=(), directory=None, *, viewer=None, environment=None):
    grove = oak.distant.tuft.cards.GROVE
    grove.CAMERAS["foreground"] = ((-26.,-13.),(-30.,-10.),32.)
    env = dict(oak.distant.environment("optimized"),MEADOW_ADAPTIVE_OAK="1",MEADOW_AA_MODE=aa)
    if environment:
        env.update(environment)
    options = [*oak.crown.GPU_FLAGS, oak.crown.REFERENCE_GUARD, "--msaa", "1" if aa in ("off","temporal") else "4"]
    if mode == "capture" and samples:
        options.extend(["--timing-samples",str(samples)])
    grove.capture(pose,mode,"combined",False,viewer=viewer or EXE,environment=env,extra_args=[*options,*extra],
        focus=json.loads((oak.distant.tuft.cards.STAGE/"scene.json").read_text())["focus"],
        output_dir=directory or OUT/"review"/aa)


if __name__ == "__main__":
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("mode",choices=("prepare","capture","view"))
    ap.add_argument("--pose",choices=oak.distant.tuft.POSES,default="canopy")
    ap.add_argument("--aa",choices=MODES,default="msaa")
    args=ap.parse_args()
    if args.mode=="prepare":
        prepare()
    else:
        render(args.mode,args.pose,args.aa)
