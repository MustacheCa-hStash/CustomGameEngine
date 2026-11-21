"""Measure frozen D3D12 oak cuts and the corresponding standalone Nanite passes."""
import argparse
import csv
import subprocess
import tempfile
from pathlib import Path
from statistics import median

import numpy as np

from BenchmarkOakGpuTraversal import capture, frozen_settings, launch_args, native_counter, score
from CompareOakMaskEdges import read_mask
from DecodeOakBoughVoxels import write_csv
from ReplayOakAssembly import BUILD, UE

OUT = BUILD / "oak-raster-profile"
TOOLS = Path(__file__).resolve().parent
CASES = [(4, 41), (1, 41), (4, 24), (1, 24), (4, 100), (1, 100)]


def rows(path):
    with path.open(newline="") as stream:
        return list(csv.DictReader(stream))


def mid(records, key):
    return median(float(r[key]) for r in records)


def d3d(repeat):
    summary, live = [], []
    settings = frozen_settings()
    for trees, distance in CASES:
        for cut, threshold in (("qualified", settings[f"{trees}/{distance}/native"]), ("nominal", 1.0)):
            exe, args = launch_args("native-gpu", trees, distance, 132, threshold)
            stem = f"{cut}-{trees}-{distance}-r{repeat}"
            with tempfile.TemporaryDirectory(dir=OUT) as folder:
                shot = Path(folder) / "frozen.png"
                subprocess.run([exe, "--capture", shot, *args, "--profile-oak-raster"], check=True, timeout=180)
                shot.replace(OUT / (stem + ".png"))
                shot.with_suffix(".mask").replace(OUT / (stem + ".mask"))
                raw = OUT / (stem + ".raster.csv")
                shot.with_suffix(".raster.csv").replace(raw)
                # Time the unmodified live path at the same clock/load, including a fresh cut each frame.
                ctl = Path(folder) / "live.png"
                subprocess.run([exe, "--capture", ctl, *args, "--timing-samples", "96"], check=True, timeout=180)
                baseline = OUT / (stem + ".live.csv")
                ctl.with_suffix(".timings.csv").replace(baseline)
            samples = rows(raw)
            identity = dict(trees=trees, distance=distance, cut=cut, repeat=repeat, error=threshold,
                            triangles=int(samples[0]["triangles"]))
            control = rows(baseline)[24:]
            if any(sum(int(r[k]) for k in ("camera_leaf_triangles", "camera_spray_wood_triangles",
                       "camera_tree_trunk_triangles")) != identity["triangles"] for r in control):
                raise ValueError(f"Live and frozen cuts disagree at {stem}")
            live.append(dict(**identity, tree_gpu_ms=mid(control, "gpu_asset_ms"),
                             select_encode_ms=mid(control, "gpu_live_cut_ms"), frame_gpu_ms=mid(control, "gpu_frame_ms"),
                             wall_ms=mid(control, "fenced_frame_ms")))
            for arm in ("depth", "visibility", "flat", "full", "early-full"):
                group = [r for r in samples if r["arm"] == arm]
                blocks = [[r for r in group if int(r["block"]) == block] for block in range(10)]
                if any(len(b) != 24 for b in blocks):
                    raise ValueError(f"Incomplete D3D raster samples at {stem}")
                times = [mid(b, "tree_gpu_ms") for b in blocks]
                summary.append(dict(**identity, arm=arm, tree_gpu_ms=median(times),
                    min_block_ms=min(times), max_block_ms=max(times),
                    frame_gpu_ms=median(mid(b, "frame_gpu_ms") for b in blocks),
                    wall_ms=median(mid(b, "fenced_ms") for b in blocks),
                    **{k: int(group[0][k]) for k in ("draws", "submissions", "covered_pixels", "vs_invocations",
                                                      "ps_invocations", "early_full_hdr_changed_pixels")}))
            print("D3D", stem, identity["triangles"], flush=True)
    return summary, live


def depth_mask(stem):
    bits = np.fromfile(stem.with_suffix(".mask"), dtype=np.uint8)
    return np.unpackbits(bits, bitorder="little").reshape(2160, 3840).astype(bool)


def grid_controls():
    result = []
    for repeat in range(2):
        for trees, distance in (CASES if repeat == 0 else reversed(CASES)):
            threshold = frozen_settings()[f"{trees}/{distance}/production"]
            exe, args = launch_args("production", trees, distance, 132, threshold)
            with tempfile.TemporaryDirectory(dir=OUT) as folder:
                shot = Path(folder) / "grid.png"
                subprocess.run([exe, "--capture", shot, *args, "--timing-samples", "96"], check=True, timeout=180)
                raw = OUT / f"grid-{trees}-{distance}-r{repeat}.csv"
                shot.with_suffix(".timings.csv").replace(raw)
            samples = rows(raw)[24:]
            tris = {sum(int(r[k]) for k in ("camera_leaf_triangles", "camera_spray_wood_triangles",
                        "camera_tree_trunk_triangles")) for r in samples}
            if len(tris) != 1:
                raise ValueError(f"Unstable grid triangle count at {trees}/{distance}")
            result.append(dict(trees=trees, distance=distance, repeat=repeat, error=threshold, triangles=tris.pop(),
                tree_gpu_ms=mid(samples, "gpu_asset_ms"), frame_gpu_ms=mid(samples, "gpu_frame_ms"),
                wall_ms=mid(samples, "fenced_frame_ms")))
    write_csv(OUT / "grid.csv", result)


def native_run(engine):
    exe = engine / "Engine/Binaries/Win64/UnrealEditor-Cmd.exe"
    project = BUILD / "unreal-nanite-oak/FoliageCreation.uproject"
    common = [str(project), "-unattended", "-RenderOffscreen", "-NoSound", "-NoSplash", "-NoP4",
              "-NoCrashDialog", "-DDC=NoShared"]
    jobs = [("materials", ["-EnablePlugins=PythonScriptPlugin,OakAssemblyBench",
                           f"-ExecutePythonScript={TOOLS / 'CreateOakRasterMaterials.py'}"], "OakRaster MATERIALS_COMPLETE"),
            ("game", ["/Game/NaniteOakBench/Benchmark", "-game", "-OakGameTiming", "-OakRasterProfile",
                      "-EnablePlugins=OakAssemblyBench", "-windowed", "-ForceRes", "-ResX=3840", "-ResY=2160",
                      "-NoVSync"], "OakGame COMPLETE")]
    for name, args, success in jobs:
        log = OUT / f"{name}.log"
        subprocess.run([exe, common[0], *args, *common[1:], f"-abslog={log}"], check=True, timeout=1200,
                       creationflags=subprocess.CREATE_NO_WINDOW, stdout=subprocess.DEVNULL)
        messages = log.read_text(encoding="utf-8", errors="replace")
        if success not in messages or any("RasterFlatSlot" in line and
            ("missing usage flag" in line or "Failed to compile" in line) for line in messages.splitlines()):
            raise RuntimeError(f"UE failed to complete {name}. See {log}")


def native_report():
    results = []
    for trees, distance in CASES:
        reference = read_mask(UE / f"gpu-oak-{trees}-{distance}-132-1.png", True)
        full = OUT / "game" / f"full-{trees}-{distance}-0"
        full_mask = depth_mask(full)
        full_hash = full.with_suffix(".sha1").read_text().strip()
        native_count = None
        for arm in ("full", "flat"):
            for repeat in range(2):
                stem = OUT / "game" / f"{arm}-{trees}-{distance}-{repeat}"
                image = stem.with_name(stem.name + "-stats.png")
                counters = [native_counter(image, y) for y in (424, 200, 216, 456)]
                if native_count is None:
                    native_count = counters
                if counters[0] != native_count[0] or counters[3] != 0:
                    raise ValueError(f"Native cut counts changed at {stem}: {counters}")
                mask = depth_mask(stem)
                if not np.array_equal(mask, full_mask) or stem.with_suffix(".sha1").read_text().strip() != full_hash:
                    raise ValueError(f"Native depth changed at {stem}")
                union = np.count_nonzero(reference | mask)
                agreement = np.count_nonzero(reference & mask) / union
                if agreement < .99:
                    raise ValueError(f"Native mask capture disagrees with the isolated 4K fixture: {stem} {agreement}")
                records = [r for r in rows(stem.with_suffix(".csv"))
                           if (r.get("FrameTime") or "").replace(".", "", 1).isdigit()][8:-5]
                if len(records) < 40:
                    raise ValueError(f"Incomplete UE samples at {stem}")
                results.append(dict(trees=trees, distance=distance, arm=arm, repeat=repeat, triangles=counters[0],
                    sw_clusters=counters[1], hw_clusters=counters[2], bricks=counters[3],
                    mask_changed_pixels=int(np.count_nonzero(mask != full_mask)), reference_iou=agreement,
                    same_depth=stem.with_suffix(".sha1").read_text().strip()==full_hash,
                    vis_ms=mid(records, "GPU/NaniteVisBuffer"), base_ms=mid(records, "GPU/NaniteBasePass"),
                    tree_ms=median(float(r["GPU/NaniteVisBuffer"])+float(r["GPU/NaniteBasePass"]) for r in records),
                    frame_gpu_ms=mid(records, "GPUTime"),
                    wall_ms=mid(rows(stem.with_name(stem.name + "-wall.csv"))[8:-5], "active_frame_ms")))
    write_csv(OUT / "nanite.csv", results)


def compare_masks():
    quality = []
    for trees, distance in CASES:
        native = depth_mask(OUT / "game" / f"full-{trees}-{distance}-0")
        fine = read_mask(UE / f"gpu-oak-{trees}-{distance}-132-0.001.png", True)
        source = read_mask(capture("source", trees, distance, 132, 1)[0])
        gate_iou, gate_edge = score(fine, read_mask(UE / f"gpu-oak-{trees}-{distance}-132-1.png", True))
        for cut in ("nominal", "qualified"):
            previous = None
            for repeat in range(2):
                actual = depth_mask(OUT / f"{cut}-{trees}-{distance}-r{repeat}")
                if previous is not None and not np.array_equal(actual, previous):
                    raise ValueError(f"D3D mask changed between independent cuts at {trees}/{distance}")
                previous = actual
                iou, edge = score(fine, actual)
                own_iou, own_edge = score(source, actual)
                quality.append(dict(trees=trees, distance=distance, cut=cut, repeat=repeat,
                    occupied=int(actual.sum()), different_pixels=int(np.count_nonzero(actual != native)),
                    native_depth_iou=score(native, actual)[0], fine_iou=iou, fine_edge_error=edge,
                    own_iou=own_iou, own_edge_error=own_edge, gate_iou=gate_iou, gate_edge_error=gate_edge,
                    passes_both=min(iou, own_iou) >= gate_iou and max(edge, own_edge) <= gate_edge))
    write_csv(OUT / "quality.csv", quality)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("part", choices=("all", "d3d", "grid", "nanite", "report"), default="all", nargs="?")
    parser.add_argument("--unreal", type=Path, default=Path("C:/Program Files/Epic Games/UE_5.8"))
    options = parser.parse_args()
    OUT.mkdir(exist_ok=True)
    if options.part in ("all", "d3d"):
        samples, controls = [], []
        for repeat in range(2):
            measured, live = d3d(repeat)
            samples.extend(measured)
            controls.extend(live)
        write_csv(OUT / "d3d.csv", samples)
        write_csv(OUT / "live.csv", controls)
    if options.part in ("all", "grid"):
        grid_controls()
    if options.part in ("all", "nanite"):
        native_run(options.unreal)
    if options.part in ("all", "nanite", "report"):
        native_report()
    if options.part in ("all", "report"):
        compare_masks()
