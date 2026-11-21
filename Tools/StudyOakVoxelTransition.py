"""Compare Voxelize and Preserve Area on the benchmark's 4K oak and grove cameras."""
import argparse
import csv
import json
import os
import subprocess
import time
import warnings
from pathlib import Path
from statistics import median

import numpy as np
from PIL import Image

from BenchmarkOakGpuTraversal import native_counter, score
from CompareOakMaskEdges import read_mask
from DecodeOakBoughVoxels import write_csv
from ReplayOakAssembly import BUILD, UE


OUT = BUILD / "oak-voxel-transition"
DISTANCES = (24, 35, 41, 50, 56, 58, 70, 100)
ANCHORS = (24, 41, 100)


def poses():
    for trees in (1, 4):
        for distance in DISTANCES:
            for yaw in ((132, 23, 197) if distance == 100 else (132,)):
                yield trees, distance, yaw


def stem(trees, distance, yaw):
    return f"voxel-study-{trees}-{distance}-{yaw}"


def jobs():
    result = []
    for trees, distance, yaw in poses():
        name = stem(trees, distance, yaw)
        base = dict(resolution=[3840,2160], vertical_fov=45, isolated=True, capture_only=True,
                    frustum=1, hzb=0, trees=trees, distance=distance, yaw=yaw,
                    pitch=-2.235 if distance==100 else -5.46, hold_frames=75)
        def add(asset, mode, suffix, error=1):
            mesh = "Tree_Optimized_" + asset
            result.append(dict(base, asset=mesh, mesh_path="/Game/NaniteOakBench/" + mesh,
                               mode=mode, name=name+"-"+suffix, max_pixels_per_edge=error))
        add("Voxels", "Voxels", "representation")
        add("Voxels", "OffscreenStats", "stats")
        add("Voxels", "Mask", "mask")
        add("Triangles", "Mask", "fine", .001)
        add("Triangles", "Mask", "triangles")
        if distance in ANCHORS and yaw == 132:
            for repeat, order in enumerate((("Voxels", "Triangles"), ("Triangles", "Voxels"))):
                for asset in order:
                    add(asset, "Profile", f"{asset}-profile-{repeat}")
    return result


def capture(engine, planned, label):
    job_file = OUT / (label+"-jobs.json")
    job_file.write_text(json.dumps(planned, indent=2))
    log = OUT / (label+".log")
    stage = UE.parent
    started = time.time_ns()
    run = subprocess.run([engine / "Engine/Binaries/Win64/UnrealEditor-Cmd.exe",
        stage / "FoliageCreation.uproject", "-EnablePlugins=PythonScriptPlugin,OakAssemblyBench",
        f"-ExecutePythonScript={Path(__file__).with_name('CaptureOakGpuComparison.py').resolve()}",
        f"-abslog={log}", "-unattended", "-RenderOffscreen", "-NoSound", "-NoSplash", "-NoP4",
        "-NoCrashDialog", "-DDC=NoShared"], env={**os.environ, "OAK_GROVE_CAPTURE_JOBS":str(job_file)},
        creationflags=subprocess.CREATE_NO_WINDOW, stdout=subprocess.DEVNULL, timeout=2400)
    output = log.read_text(errors="replace", encoding="utf-8")
    if "OAK_CAPTURE CAPTURES_COMPLETE" not in output or log.stat().st_mtime_ns < started:
        raise RuntimeError(f"Native capture did not complete. See {log}")
    for job in planned:
        name = job["name"]
        result = (stage/"Saved/Profiling/CSV"/(name+".csv") if job["mode"] == "Profile"
                  else UE/(name+".png"))
        if f"OAK_CAPTURE Captured {name}" not in output or result.stat().st_mtime_ns < started:
            raise RuntimeError(f"No fresh capture for {name}. See {log}")
    if run.returncode:
        teardown = output.split("OAK_CAPTURE CAPTURES_COMPLETE", 1)[1]
        if (run.returncode & 0xffffffff) != 0xc0000005 or "LogExit: Exiting." not in teardown or "Log file closed" not in teardown:
            run.check_returncode()
        warnings.warn(f"UE exited with 0xc0000005 after closing its log. All {len(planned)} new outputs will be validated. See {log}")


def profile(name):
    path = UE.parent/"Saved/Profiling/CSV"/(name+".csv")
    with path.open() as f:
        samples = [r for r in csv.DictReader(f) if (r.get("FrameTime") or "").replace(".", "", 1).isdigit()][8:-5]
    if len(samples) < 40:
        raise ValueError(f"Incomplete UE timing at {path}")
    vis = [float(r["GPU/NaniteVisBuffer"]) for r in samples]
    base = [float(r["GPU/NaniteBasePass"]) for r in samples]
    return dict(samples=len(base), visibility_ms=median(vis), base_ms=median(base),
                vis_base_ms=median(a+b for a,b in zip(vis,base)))


def representation(image, mask):
    with Image.open(image) as im:
        red, green, _ = np.asarray(im.convert("RGB")).transpose(2,0,1)
    foreground = red != green
    occupied = int(foreground.sum())
    if not occupied:
        raise ValueError(f"No visible oak pixels in {image}")
    agreement = np.count_nonzero(foreground & mask) / np.count_nonzero(foreground | mask)
    if agreement < .995:
        raise ValueError(f"Voxel visualization disagrees with the selected mask at {image}: {agreement}")
    voxel_pixels = int((green > red).sum())
    return dict(occupied=occupied, voxel_pixels=voxel_pixels, voxel_share=voxel_pixels/occupied,
                representation_iou=agreement)


def match_far(engine, summarize_only=False, reuse_sweep=False):
    trials = (.875, .75, .625, .5, .48, .46, .44, .42, .40, .375, .25)
    cameras = [j for j in jobs() if j["distance"]==100 and j["name"].endswith("-mask")]
    def candidate(job, threshold):
        return f'{job["name"]}-match-{threshold}'
    sweep = [dict(j, name=candidate(j,x), max_pixels_per_edge=x) for j in cameras for x in trials]
    if not summarize_only and not reuse_sweep:
        capture(engine, sweep, "match-100-sweep")
    rows, selected = [], {}
    for j in cameras:
        root = stem(j["trees"], 100, j["yaw"])
        ref = read_mask(UE/(root+"-fine.png"), True)
        ti, te = score(ref, read_mask(UE/(root+"-triangles.png"), True))
        for x in trials:
            vi, ve = score(ref, read_mask(UE/(candidate(j,x)+".png"), True))
            rows.append(dict(trees=j["trees"],yaw=j["yaw"],error=x,iou=vi,edge=ve,
                             triangle_iou=ti,triangle_edge=te,passed=vi>=ti and ve<=te))
    write_csv(OUT/"match-100-quality.csv", rows)
    for trees in (1,4):
        passed = [x for x in trials if all(r["passed"] for r in rows if r["trees"]==trees and r["error"]==x)]
        if passed:
            selected[trees] = max(passed)
    measurements = []
    for j in cameras:
        trees = j["trees"]
        if trees not in selected:
            continue
        x = selected[trees]
        root = candidate(j,x)
        measurements.append(dict(j, mode="Voxels", name=root+"-rep", max_pixels_per_edge=x))
        measurements.append(dict(j, mode="OffscreenStats", name=root+"-stats", max_pixels_per_edge=x))
        if j["yaw"] == 132:
            triangle = "Tree_Optimized_Triangles"
            measurements.append(dict(j, asset=triangle, mesh_path="/Game/NaniteOakBench/"+triangle,
                mode="OffscreenStats", name=root+"-triangle-stats", max_pixels_per_edge=1))
            arms = (("triangles","Triangles",1),("nominal","Voxels",1),("qualified","Voxels",x))
            for repeat,order in enumerate((arms, reversed(arms))):
                for arm,asset,threshold in order:
                    mesh = "Tree_Optimized_"+asset
                    measurements.append(dict(j, asset=mesh, mesh_path="/Game/NaniteOakBench/"+mesh,
                        mode="Profile", name=f"{root}-{arm}-{repeat}", max_pixels_per_edge=threshold))
    if not measurements:
        raise RuntimeError("No tested voxel asset setting met the 100 m triangle silhouette gates.")
    if not summarize_only:
        capture(engine, measurements, "match-100-profile")
    costs, cuts = [], []
    for j in cameras:
        trees = j["trees"]
        if trees not in selected:
            continue
        x = selected[trees]
        root = candidate(j,x)
        mask = read_mask(UE/(root+".png"), True)
        stats = UE/(root+"-stats.png")
        cuts.append(dict(trees=trees,yaw=j["yaw"],error=x,triangles=native_counter(stats,424),
                         bricks=native_counter(stats,456),triangle_control=(
                             native_counter(UE/(root+"-triangle-stats.png"),424) if j["yaw"]==132 else None),
                         **representation(UE/(root+"-rep.png"),mask)))
        if j["yaw"] == 132:
            for repeat in (0,1):
                for arm,asset,threshold in (("triangles","Triangles",1),("nominal","Voxels",1),("qualified","Voxels",x)):
                    costs.append(dict(trees=trees,cut=arm,asset=asset,error=threshold,repeat=repeat,
                                      **profile(f"{root}-{arm}-{repeat}")))
    write_csv(OUT/"match-100-cuts.csv", cuts)
    write_csv(OUT/"match-100-cost.csv", costs)


def summarize():
    costs, quality = [], []
    for trees, distance, yaw in poses():
        name = stem(trees, distance, yaw)
        stats = UE / (name+"-stats.png")
        mask = read_mask(UE / (name+"-mask.png"), True)
        fine = read_mask(UE / (name+"-fine.png"), True)
        triangles = read_mask(UE / (name+"-triangles.png"), True)
        vi, ve = score(fine, mask)
        ti, te = score(fine, triangles)
        quality.append(dict(trees=trees, distance=distance, yaw=yaw,
            surface_triangles=native_counter(stats,424), bricks=native_counter(stats,456),
            **representation(UE/(name+"-representation.png"),mask),
            voxel_iou=vi, voxel_edge=ve, triangle_iou=ti, triangle_edge=te,
            passes_triangle_gate=vi>=ti and ve<=te))
        if distance not in ANCHORS or yaw != 132:
            continue
        for asset in ("Triangles", "Voxels"):
            for repeat in (0,1):
                costs.append(dict(trees=trees, distance=distance, asset=asset, repeat=repeat,
                                  **profile(f"{name}-{asset}-profile-{repeat}")))
    write_csv(OUT/"transition.csv", quality)
    write_csv(OUT/"cost.csv", costs)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--unreal", type=Path, default=Path("C:/Program Files/Epic Games/UE_5.8"))
    parser.add_argument("--summarize-only", action="store_true", help="Score existing captures without launching Unreal")
    parser.add_argument("--match-100", action="store_true", help="Find and profile settings meeting three triangle silhouette gates at 100 m using the base study references")
    parser.add_argument("--reuse-sweep", action="store_true", help="Profile a completed 100 m threshold sweep again")
    options = parser.parse_args()
    OUT.mkdir(exist_ok=True)
    if options.match_100 or options.reuse_sweep:
        match_far(options.unreal, options.summarize_only, options.reuse_sweep)
    else:
        if not options.summarize_only:
            capture(options.unreal, jobs(), "capture")
        summarize()
