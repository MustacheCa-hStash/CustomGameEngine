"""Check the default oak review lighting at a fixed grove camera and beneath the canopy."""
import csv
import subprocess
import tempfile
from statistics import median
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw, ImageFont

from BenchmarkOakGpuTraversal import frozen_settings, launch_args
from DecodeOakBoughVoxels import write_csv
from ReplayOakAssembly import BUILD

OUT = BUILD / "oak-grove-lighting"


def capture(label, exe, base, extra):
    with tempfile.TemporaryDirectory(dir=OUT) as directory:
        shot = Path(directory) / "frame.png"
        subprocess.run([exe, "--capture", shot, *base, *extra, "--timing-samples", "64"],
                       check=True, timeout=180)
        shot.replace(OUT / (label+".png"))
        timings = OUT / (label+".csv")
        shot.with_suffix(".timings.csv").replace(timings)
    with timings.open() as f:
        samples = list(csv.DictReader(f))[16:]
    geometry = {tuple(int(r[k]) for k in ("camera_leaf_triangles","camera_spray_wood_triangles",
        "camera_tree_trunk_triangles","shadow_triangles")) for r in samples}
    if len(geometry) != 1:
        raise ValueError(f"Unstable oak cut in {label}")
    with Image.open(OUT/(label+".png")) as im:
        rgb = np.asarray(im.convert("RGB"))
    return rgb, geometry.pop(), median(float(r["gpu_asset_ms"]) for r in samples)


def main():
    OUT.mkdir(exist_ok=True)
    report = []
    canvas = Image.new("RGB",(1440,628),"#20252a")
    draw = ImageDraw.Draw(canvas)
    font = ImageFont.truetype("segoeui.ttf",20)
    for row, (pose, trees, distance) in enumerate((("grove",4,41),("under",1,7))):
        error = frozen_settings()["4/41/native"] if pose=="grove" else 1.
        exe, base = launch_args("native-gpu",trees,distance,132,error,probe_gi=None)
        if pose == "under":
            for flag, value in (("--camera","under-canopy"),("--yaw-degrees","-37.24"),("--pitch-degrees","-21")):
                base[base.index(flag)+1] = value
        variants = {"material":["--geometry-mask","--bough-materials"], "previous":["--probe-gi","off"],
            "filtered":["--probe-gi","sky"], "default":[], "explicit-bounce":["--probe-gi","bounce"],
            "transmitted":["--lighting","transmission"], "zero":["--lighting","transmission","--backlighting","0"]}
        frames, counts, timings = {}, {}, {}
        for tag, extra in variants.items():
            frames[tag], counts[tag], timings[tag] = capture(pose+"-"+tag,exe,base,extra)
        if len(set(counts.values())) != 1:
            raise ValueError(f"Lighting changed submitted geometry in {pose}")
        material = frames["material"]
        leaves, wood = material[:,:,1]>material[:,:,0], material[:,:,0]>material[:,:,1]
        canopy = leaves | wood
        def changed(a,b):
            difference = np.abs(frames[a].astype(np.int16)-frames[b].astype(np.int16)).max(axis=2)
            return np.count_nonzero(canopy & (difference>1))/canopy.sum()
        changed_sky, changed_bounce = changed("previous","filtered"), changed("filtered","default")
        reference_difference = changed("default","explicit-bounce")
        lit = frames["transmitted"].max(axis=2)>1
        transmission = np.count_nonzero(lit & leaves)/leaves.sum()
        if reference_difference>.001 or min(changed_sky,changed_bounce,transmission)<.01 or frames["zero"].any():
            raise ValueError(f"Lighting check failed in {pose}: {changed_sky}, {changed_bounce}, {transmission}, {reference_difference}")
        report.append(dict(pose=pose, trees=trees, distance=distance, triangles=sum(counts["default"][:3]),
            shadow_triangles=counts["default"][3], leaf_pixels=int(leaves.sum()), wood_pixels=int(wood.sum()),
            sky_filter_changed_fraction=changed_sky, ground_bounce_changed_fraction=changed_bounce,
            default_vs_explicit_changed_fraction=reference_difference, transmitting_leaf_fraction=transmission,
            zero_transmission_nonblack_pixels=int((frames["zero"].max(axis=2)>0).sum()),
            probes_off_tree_ms=timings["previous"], lit_tree_ms=timings["default"]))
        for column,(tag,title) in enumerate((("previous","Open skylight"),("default","Canopy sky + ground bounce"),
                                            ("transmitted","Transmitted light"))):
            x,y=480*column,314*row
            canvas.paste(Image.fromarray(frames[tag]).resize((480,270),Image.Resampling.LANCZOS),(x,y+40))
            draw.text((x+10,y+7),("41 m grove" if row==0 else "Under canopy")+"  /  "+title,font=font,fill="white")
    write_csv(OUT/"checks.csv",report)
    canvas.save(OUT/"comparison.png")


if __name__=="__main__":
    main()
