"""Reposition connected upper limbs on the varied-spray oak."""
import argparse
import json
import shutil
from pathlib import Path

import numpy as np

import sprays
from sprays import oak


OUT = oak.lighting.ROOT / "build/oak-upper-crown"
MODEL = sprays.MODEL.with_name("WhiteOak-OpenCrown.glb")

# Source skeleton branch ID, distance over which to bend, distal shift in metres.
# Bend the secondary limbs before their common parent carries them into place.
BENDS = [
    (5417, 1.60, (-1.18, -0.51, -1.05)),
    (5444, 1.25, (0.50, -0.57, 1.05)),
    (5501, 1.32, (1.42, -0.70, 0.70)),
    (5566, 0.92, (0.16, -0.53, 1.40)),
    (5599, 1.05, (-0.75, 0.24, -0.53)),
    (11, 3.95, (-1.16, -0.65, -0.65)),
    (16, 4.70, (0.94, -0.10, 0.45)),
]


def branch_map():
    """Recover the retained bark vertex and foliage owners in the remodel's order."""
    skeleton = json.loads(oak.SKELETON.read_text())
    paths = skeleton["primitives"]["points"]
    points = np.asarray(skeleton["points"]["positions"])
    ancestry = skeleton["primitives"]["attributes"]["parents"]["values"]
    original = oak.OakGlb(oak.lighting.SCENE / "assets" / (oak.NAME + ".glb"))
    bark = original.doc["meshes"][0]["primitives"][0]
    attr = bark["attributes"]
    triangles = original.array(bark["indices"]).reshape(-1, 3)
    grafts = original.doc["nodes"][1]["extensions"]["EXT_mesh_gpu_instancing"]["attributes"]
    wood, foliage = oak.branch_owners(original.array(attr["POSITION"]), triangles,
        original.array(attr["TEXCOORD_2"])[:, 0], points, paths, original.array(grafts["TRANSLATION"]))
    rotations, offsets, sizes, keep = oak.structure(skeleton, 5)
    used, remap = np.unique(triangles[keep[wood[triangles[:, 0]]]], return_inverse=True)
    base = oak.OakGlb(oak.MODEL)
    previous_bark = base.doc["meshes"][0]["primitives"][0]
    if not np.array_equal(remap.ravel(), base.array(previous_bark["indices"]).ravel()):
        raise ValueError("The source skeleton does not match the retained bark.")
    inst = base.doc["nodes"][1]["extensions"]["EXT_mesh_gpu_instancing"]["attributes"]
    positions = base.array(inst["TRANSLATION"])
    graft_owners = {}
    for pos, owner in zip(positions, foliage[keep[foliage]], strict=True):
        graft_owners[tuple(pos)] = owner
    if len(graft_owners) != len(positions):
        raise ValueError("Graft positions are ambiguous.")

    bends = []
    for branch, reach, delta in BENDS:
        i = branch - 1
        members = np.array([j == i or branch in chain for j, chain in enumerate(ancestry)])
        ends = points[[paths[i][0], paths[i][-1]]]
        root, tip = offsets[i] + sizes[i] * oak.rotate(rotations[i], ends)
        bends.append((members, root, sprays.unit(tip - root), reach, np.array(delta)))
    return wood[used], graft_owners, bends


def bend(points, frames, owners, bends, is_bark):
    for members, root, axis, reach, shift in bends:
        mask = members[owners]
        t = np.clip(((points[mask] - root) @ axis) / reach, 0, 1)
        # Zero slope at graft and tip keeps both the join and distal tuft intact.
        weight = (t*t*(3 - 2*t))[:, None]
        slope = (6*t*(1 - t)/reach)[:, None]
        points[mask] += weight * shift
        # The deformation's Jacobian is I + slope * outer(shift, axis).
        if is_bark:
            n = frames[mask]
            correction = slope * (n @ shift)[:, None] / (1 + slope * (axis @ shift))
            frames[mask] = sprays.unit(n - correction * axis)
        else:
            q = frames[mask]
            direction = oak.rotate(q, np.array([0., 1., 0.]))
            bent = sprays.unit(direction + slope * (direction @ axis)[:, None] * shift)
            swivel = sprays.unit(np.column_stack((np.cross(direction, bent), 1 + (direction*bent).sum(axis=1))))
            frames[mask] = sprays.unit(oak.compose(swivel, q))
    return points, frames


def author(preview_asset, source_preview):
    wood, graft_owners, bends = branch_map()
    glb = oak.OakGlb(sprays.MODEL)
    attr = glb.doc["meshes"][0]["primitives"][0]["attributes"]
    bark, normals = bend(glb.array(attr["POSITION"]), glb.array(attr["NORMAL"]), wood, bends, True)
    glb.replace(attr["POSITION"], bark)
    glb.replace(attr["NORMAL"], normals)
    for node in glb.doc["nodes"][1:]:
        inst = node["extensions"]["EXT_mesh_gpu_instancing"]["attributes"]
        p, q = glb.array(inst["TRANSLATION"]), glb.array(inst["ROTATION"])
        owners = np.array([graft_owners[tuple(pos)] for pos in p])
        p, q = bend(p, q, owners, bends, False)
        glb.replace(inst["TRANSLATION"], p)
        glb.replace(inst["ROTATION"], q)
    glb.doc["scenes"][0]["name"] = "White oak with separated upper branches"
    glb.write(MODEL)

    # Both viewers use the same existing per-form index templates.
    preview = oak.OakGlb(source_preview)
    for full_mesh, preview_mesh in zip(glb.doc["meshes"][1:], preview.doc["meshes"][1:], strict=True):
        for full, reduced in zip(full_mesh["primitives"], preview_mesh["primitives"], strict=True):
            glb.replace(full["indices"], preview.array(reduced["indices"]))
    glb.write(preview_asset)


def stage():
    control = sprays.prepare()
    exe = sprays.prepare_transmission(control)
    scene = OUT / "scene"
    assets = scene / "assets"
    assets.mkdir(parents=True, exist_ok=True)
    for source in [exe, *(control / "assets").glob("MeadowDaySky.*")]:
        dest = (scene if source == exe else assets) / source.name
        if not dest.exists() or dest.stat().st_mtime_ns != source.stat().st_mtime_ns:
            shutil.copy2(source, dest)
    preview = assets / (oak.NAME + ".glb")
    source_preview = control / "assets" / preview.name
    inputs = (Path(__file__), sprays.MODEL, source_preview, oak.MODEL, oak.SKELETON)
    if (not preview.exists() or not MODEL.exists() or
            min(preview.stat().st_mtime_ns, MODEL.stat().st_mtime_ns) < max(p.stat().st_mtime_ns for p in inputs)):
        author(preview, source_preview)
    return scene


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--pose", choices=sprays.VIEWS, default="fork")
    parser.add_argument("--trees", type=int, choices=(1, 4), default=1)
    parser.add_argument("--control", action="store_true", help="View the previous varied-spray crown")
    parser.add_argument("--capture", type=Path)
    args = parser.parse_args()
    scene = sprays.prepare() if args.control else stage()
    sprays.render(scene, args.pose, args.capture, args.trees, exe=scene / sprays.SPRAY_EXE)
