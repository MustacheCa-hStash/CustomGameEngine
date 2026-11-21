"""Refit exposed bough 7 at the existing 3.8 cm leaf brick budget, allowing locations to change."""
import argparse
import csv
import json
import struct
import subprocess
from statistics import median

import numpy as np

from FitOakBoughVoxels import EXE, ROOT, VIEWS, key, rays, training, write_bake
from DecodeOakBoughVoxels import write_csv

LABEL = "relocated-38-leaf"
OUT = ROOT.parent / "oak-voxel-transition"
BRICK = np.dtype([("p", "<i4", 3), ("mask", "<u8")])


def bake():
    source = ROOT / "merged-38-0.samples"
    cells = np.fromfile(source, dtype=[("p","<i4",3), ("coverage","<f4"), ("normal","<f4",3)])
    p = cells["p"]
    blocks, inverse = np.unique(p // 4, axis=0, return_inverse=True)
    inverse = inverse.ravel()
    bits = (p % 4) @ np.array([1,4,16])
    flags = np.left_shift(np.uint64(1), bits.astype(np.uint64))
    cell_ids = inverse * 64 + bits
    eligible = np.zeros(len(blocks), np.uint64)
    np.bitwise_or.at(eligible, inverse, flags)
    previous = np.fromfile(ROOT / "merged-38-0.optimized", dtype=BRICK)
    seed = np.zeros_like(eligible)
    seed[np.searchsorted(key(blocks), key(previous["p"]))] = previous["mask"]
    views = []
    for yaw in VIEWS:
        target, crop = training(yaw, 0)
        pairs = np.concatenate(list(rays(p, .038, cell_ids, yaw, crop)))
        views.append((target.ravel(), pairs))

    def fit(locations, start):
        # Map from the full sampled grid to the compact list accepted by the existing fitter.
        ids = np.cumsum(locations) - 1
        path = OUT / "bough7-locations.fit"
        with path.open("wb") as stream:
            stream.write(struct.pack("<3I", locations.sum(), locations.sum(), len(views)))
            for coordinate, mask, initial in zip(blocks[locations], eligible[locations], start[locations]):
                stream.write(struct.pack("<3iQQ", *coordinate, mask, initial))
            for target, pairs in views:
                selected = pairs[locations[pairs[:,0] // 64]].copy()
                selected[:,0] = ids[selected[:,0] // 64] * 64 + selected[:,0] % 64
                stream.write(struct.pack("<2I", target.size, len(selected)))
                stream.write(target.astype("<u2").tobytes())
                stream.write(selected.tobytes())
        result = OUT / "bough7-locations.optimized"
        subprocess.run([EXE, "fit", path, result], check=True)
        packed = np.fromfile(result, dtype=BRICK)
        masks = np.zeros_like(eligible)
        masks[np.searchsorted(key(blocks), key(packed["p"]))] = packed["mask"]
        return masks

    # Explore all source-intersected locations using only the original seven training yaws.
    free = fit(np.ones(len(blocks), bool), seed)
    value = np.zeros(len(blocks))
    for target, pairs in views:
        block, bit = np.divmod(pairs[:,0], 64)
        used = ((free[block] >> bit.astype(np.uint64)) & 1) != 0
        # Divide a pixel's credit among all bricks covering it. Multiple cells of one brick vote once.
        touches = np.unique(block[used].astype(np.int64) * target.size + pairs[used,1])
        brick, pixel = np.divmod(touches, target.size)
        overlap = np.bincount(pixel, minlength=target.size)
        credit = np.where(target[pixel], 1., -1.) / (overlap[pixel] * target.sum())
        value += np.bincount(brick, weights=credit, minlength=len(blocks))
    candidates = np.flatnonzero(free)
    chosen = candidates[np.argsort(-value[candidates], kind="stable")[:len(previous)]]
    allowed = np.zeros(len(blocks), bool)
    allowed[chosen] = True
    final = fit(allowed, free)
    order = np.argsort(cell_ids)
    keep = order[(final[inverse[order]] & flags[order]) != 0]
    groups = np.split(keep, np.flatnonzero(np.diff(inverse[keep]))+1)
    bricks = []
    for group in groups:
        b = inverse[group[0]]
        frame = np.vstack((blocks[b]*4*.038, np.eye(3)*.038))
        bricks.append((frame, int(final[b]), cells["normal"][group]))
    write_bake(ROOT / (LABEL+".oakvx"), [bricks, []], 1)
    (OUT/"relocation.json").write_text(json.dumps(dict(cell_m=.038, training_yaws=VIEWS,
        old_bricks=len(previous), new_bricks=len(bricks), eligible_locations=len(blocks),
        new_locations=int(np.count_nonzero((seed==0) & (final!=0)))), indent=2))


def validate():
    from BenchmarkOakBoughVoxels import stage, shot, scores, read_mask
    from ValidateOakBoughVoxels import decoded, pixels
    view = stage("pilot-view","pilot-7")
    with (ROOT/"validation.csv").open() as stream:
        controls = [r for r in csv.DictReader(stream) if r["cut"] == "merged-38-fit-leaf"]
    if len(controls) != 13 or len({int(r["yaw"]) for r in controls}) != len(controls):
        raise ValueError("The 13-angle bough qualification fixture is incomplete.")
    rows = []
    for control in controls:
        yaw = int(control["yaw"])
        image, work = shot(view, f"{LABEL}-{yaw}", LABEL, yaw=yaw)
        q = scores(image, read_mask(ROOT/"captures"/f"ref-{yaw}.png"))
        rows.append(dict(yaw=yaw, held_out=yaw not in VIEWS, bricks=work["bricks"], residual=work["source_tris"],
            old_iou=control["iou"], new_iou=q["iou"], old_edge=control["edge"], new_edge=q["edge"],
            passes_triangle=q["iou"]>=float(control["triangle_gate_iou"]) and q["edge"]<=float(control["triangle_gate_edge"]),
            passes_voxel=q["iou"]>=float(control["voxel_gate_iou"]) and q["edge"]<=float(control["voxel_gate_edge"])))
    write_csv(OUT/"relocation-quality.csv", rows)
    dep, _ = shot(view, LABEL+"-depth", LABEL, depth=True)
    mat, _ = shot(view, LABEL+"-materials", LABEL, extra=["--bough-materials"])
    ref = ROOT/"captures"
    def labels(path):
        rgb = pixels(path)
        return np.sign(rgb[:,:,1].astype(int)-rgb[:,:,0].astype(int))
    truth, candidate = labels(ref/"materials-source.png"), labels(mat)
    shared = (truth!=0) & (candidate!=0)
    depth = np.abs(decoded(ref/"depth-source.png")-decoded(dep))
    metrics = dict(p95_depth_m=float(np.percentile(depth[shared],95)))
    for name, slot in (("leaf",1),("twig",-1)):
        metrics[name+"_iou"] = np.count_nonzero((truth==slot)&(candidate==slot))/np.count_nonzero((truth==slot)|(candidate==slot))
    (OUT/"relocation-first-hit.json").write_text(json.dumps(metrics,indent=2))


def time_and_light():
    from BenchmarkOakBoughVoxels import BASE, stage, shot
    from ValidateOakBoughVoxels import pixels
    crown_budget = json.loads((BASE/"crown-chosen.json").read_text())["pilot-100"]["budget"]
    controls = [("triangles", None), ("previous", "merged-38-fit-leaf"), ("relocated", LABEL)]
    results = []
    for scene, fixture, budget in (("bough", "pilot-7", 1), ("crown", "pilot", crown_budget)):
        view = stage(scene + "-timing-view", fixture)
        for repeat, order in enumerate((controls, reversed(controls))):
            for arm, sidecar in order:
                image, work = shot(view, f"location-time-{scene}-{arm}-{repeat}", sidecar, shaded=True,
                                   crown=scene=="crown", budget=budget, extra=["--probe-gi","bounce"])
                with image.with_suffix(".timings.csv").open() as f:
                    samples = list(csv.DictReader(f))[16:]
                if len(samples) != 45 or bool(sidecar) != bool(work["bricks"]):
                    raise ValueError(f"Invalid {scene} timing for {arm}")
                results.append(dict(scene=scene, cut=arm, repeat=repeat, samples=len(samples),
                    residual_triangles=work["source_tris"], bricks=work["bricks"],
                    camera_ms=median(float(r["gpu_camera_ms"]) for r in samples),
                    gpu_frame_ms=median(float(r["gpu_frame_ms"]) for r in samples)))
    write_csv(OUT/"relocation-cost.csv", results)

    # All leaves of this isolated bough use the candidate. Wood remains independently identifiable.
    view = stage("pilot-view", "pilot-7")
    material, _ = shot(view, "location-light-material", LABEL, extra=["--bough-materials"])
    rgb = pixels(material)
    leaf = rgb[:,:,1] > rgb[:,:,0]
    images = {}
    for name, mode, strength in (("open","off",None), ("bounce","bounce",None), ("zero","bounce","0")):
        flags = ["--lighting","transmission","--probe-gi",mode]
        if strength is not None:
            flags.extend(["--backlighting",strength])
        path, _ = shot(view, "location-light-"+name, LABEL, shaded=True, extra=flags)
        images[name] = pixels(path)[leaf].astype(np.int16)
    lit = np.any(images["bounce"] > 1, axis=1)
    changed = np.any(np.abs(images["open"]-images["bounce"]) > 1, axis=1)
    zero = int(np.count_nonzero(images["zero"]))
    if not lit.any() or not changed.any() or zero:
        raise ValueError("Bough leaf brick transmission or probe lighting check failed.")
    (OUT/"brick-lighting.json").write_text(json.dumps(dict(leaf_pixels=int(leaf.sum()),
        transmitting=int(lit.sum()), probe_changed=int(changed.sum()), nonzero_channels_at_zero=zero), indent=2))


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("action", choices=("bake","validate","timing","all"), nargs="?", default="all")
    action = parser.parse_args().action
    OUT.mkdir(exist_ok=True)
    if action in ("bake","all"): bake()
    if action in ("validate","all"): validate()
    if action in ("timing","all"): time_and_light()
