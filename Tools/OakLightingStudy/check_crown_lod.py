"""Matched 4K color, silhouette, motion and GPU checks for the open-crown bake."""
import argparse
import csv
from pathlib import Path
from statistics import median

import numpy as np
from PIL import Image

import optimize_crown as opt
from CompareOakMaskEdges import measure, read_mask


STILLS = [
    ("fork", "fork", 1, 24, 132), ("crown", "crown", 1, 28, 197),
    ("grove", "grove", 4, 41, 132), ("side", "grove", 4, 41, 23),
    ("far100", "grove", 4, 100, 197), ("under", "sunward", 1, 7.6, -76),
    ("edge", "edge", 1, 3.2, -96),
]
PATHS = [("approach", 41, 100, 23, 31, 16), ("holdout", 50, 56, 197, -12, 8),
         ("far-orbit", 100, 175, 132, 26, 12), ("walk-speed", 50, 56, 276, 5, 121)]


def write_csv(path, rows):
    with path.open("w", newline="") as f:
        writer = csv.DictWriter(f, fieldnames=rows[0])
        writer.writeheader()
        writer.writerows(rows)


def check(out, budget, references=None, unrestricted=False):
    out.mkdir(exist_ok=True, parents=True)
    source = opt.OUT / "full"
    (source / "assets").mkdir(parents=True, exist_ok=True)
    opt.copy_changed(opt.crown.MODEL, source / "assets" / opt.ASSET.name)
    previous = opt.crown.OUT / "scene"
    for original in [previous / opt.sprays.SPRAY_EXE, *(previous / "assets").glob("MeadowDaySky.*")]:
        opt.copy_changed(original, (source if original.suffix == ".exe" else source / "assets") / original.name)
    flags = [*opt.GPU_FLAGS, "--spray-pixel-error", str(budget)]
    if not unrestricted:
        flags.append(opt.REFERENCE_GUARD)
    arms = [("source", source, []), ("preview", previous, []), ("gpu", opt.SCENE, flags)]
    def render(arm, pose, trees, flags, dest):
        name, scene, args = arm
        if references and name != "gpu":
            dest = references / dest.name
        if not dest.exists():
            opt.sprays.render(scene, pose, dest, trees, [*args, *flags], scene / opt.sprays.SPRAY_EXE)
        return dest

    quality, timing = [], []
    for key, pose, trees, distance, yaw in STILLS:
        flags = ["--distance", str(distance), "--yaw-degrees", str(yaw)]
        if distance >= 100:
            flags.extend(["--pitch-degrees","-2.235"])
        for arm in arms:
            name = arm[0]
            mask = read_mask(render(arm, pose, trees, [*flags, "--geometry-mask"], out/f"{name}-{key}-mask.png"))
            if name == "source":
                ref = mask
            _, _, iou, _, _, edge = next(measure(ref, [(0, 0, mask)]))
            counts = []
            for repeat in range(2 if name != "source" and key in ("crown","grove","far100") else 1):
                file = out/f"{name}-{key}-r{repeat}.png"
                file = render(arm, pose, trees, [*flags, "--timing-samples", "9" if name=="source" else "21"], file)
                with file.with_suffix(".timings.csv").open() as stream:
                    samples = list(csv.DictReader(stream))
                counts.append({col: median(float(s[col]) for s in samples) for col in
                               ("camera_triangles","shadow_triangles","gpu_asset_ms","gpu_frame_ms","fenced_frame_ms")})
                if repeat == 0:
                    rgb = np.asarray(Image.open(file).convert("RGB"))
                    if name == "source":
                        source_rgb = rgb
                    delta = np.abs(rgb[ref | mask].astype(np.int16) - source_rgb[ref | mask])
                    quality.append(dict(view=key, arm=name, iou=iou, edge_miss=edge, mean_rgb_error_8bit=delta.mean()))
            timing.append(dict(view=key, arm=name, **{k:median(r[k] for r in counts) for k in counts[0]}))
        write_csv(out/"stills.csv", quality)
        write_csv(out/"timing.csv", timing)
        print("Checked",key,flush=True)
    motion = []
    for key, near, far, yaw, orbit, frames in PATHS:
        flags = ["--geometry-mask", "--distance", str(near), "--end-distance", str(far),
                 "--yaw-degrees", str(yaw), "--orbit-degrees", str(orbit), "--frames", str(frames),
                 "--animate-wind", "--wind-strength", "0.35"]
        if far >= 100:
            flags.extend(["--end-pitch-degrees","-2.235"])
        if near >= 100:
            flags.extend(["--pitch-degrees","-2.235"])
        folders = [render(arm,"grove",4,flags,out/f"{key}-{arm[0]}") for arm in arms]
        last = {}
        for i in range(frames):
            for arm,folder in zip(arms,folders):
                name = arm[0]
                mask = read_mask(folder/f"frame-{i:06}.png")
                if name == "source":
                    ref = mask
                _, _, iou, _, _, edge = next(measure(ref, [(0,0,mask)]))
                temporal = ""
                if i:
                    changed = (ref ^ prev_ref) ^ (mask ^ last[name])
                    temporal = changed.sum() / (ref | mask | prev_ref | last[name]).sum()
                motion.append(dict(path=key,frame=i,arm=name,iou=iou,edge_miss=edge,temporal=temporal))
                last[name] = mask
            prev_ref = ref
        write_csv(out/"motion.csv",motion)
        print("Checked",key,flush=True)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output",type=Path,required=True)
    parser.add_argument("--budget",type=float,default=1)
    parser.add_argument("--references",type=Path)
    parser.add_argument("--unrestricted",action="store_true")
    args=parser.parse_args()
    check(args.output,args.budget,args.references,args.unrestricted)
