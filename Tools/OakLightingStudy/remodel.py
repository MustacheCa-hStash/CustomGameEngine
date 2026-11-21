"""Author and inspect a forked white oak on a private copy of the lighting-study scene."""
import argparse
import json
import os
from pathlib import Path
import shutil
import struct
import subprocess
import tempfile

import numpy as np
import run as lighting


OUT = lighting.ROOT / "build/oak-crown-remodel"
NAME = "WhiteOak-ForkedCanopy"
SKELETON = lighting.ROOT / "build/oak-pve-forked-test/skeleton/WhiteOak.json"
MODEL = lighting.ROOT / "assets/source/white-oak-crown-study/WhiteOak-OpenFork.glb"
POSES = {"fork": (24, 132, -5.46), "reverse": (24, 23, -5.46),
         "three-quarter": (28, 197, -4), "grove": (41, 132, -5.46), "rim": (5, -78, -40),
         "backlight": (4, -95, -40)}


class OakGlb:
    def __init__(self, path):
        data = path.read_bytes()
        size, = struct.unpack_from("<I", data, 12)
        self.doc = json.loads(data[20:20+size])
        self.payload = bytearray(data[28+size:])

    def array(self, index):
        a = self.doc["accessors"][index]
        view = self.doc["bufferViews"][a["bufferView"]]
        dtype = np.dtype({5123: "<u2", 5125: "<u4", 5126: "<f4"}[a["componentType"]])
        width = {"SCALAR": 1, "VEC2": 2, "VEC3": 3, "VEC4": 4}[a["type"]]
        return np.ndarray((a["count"], width), dtype, self.payload,
                          view.get("byteOffset", 0) + a.get("byteOffset", 0),
                          (view.get("byteStride", width * dtype.itemsize), dtype.itemsize)).copy()

    def replace(self, index, values):
        a = self.doc["accessors"][index]
        original = self.array(index)
        values = np.ascontiguousarray(values, dtype=original.dtype).reshape(-1, original.shape[1])
        v = self.doc["bufferViews"][a["bufferView"]]
        assert "byteStride" not in v and a.get("byteOffset", 0) == 0
        raw = values.tobytes()
        assert len(raw) <= v["byteLength"]
        start = v.get("byteOffset", 0)
        self.payload[start:start+len(raw)] = raw
        a["count"] = len(values)
        if "min" in a:
            a.update(min=values.min(axis=0).tolist(), max=values.max(axis=0).tolist())

    def write(self, path):
        header = json.dumps(self.doc, separators=(",", ":")).encode()
        header += b" " * (-len(header) % 4)
        with tempfile.NamedTemporaryFile(dir=path.parent, delete=False) as f:
            f.write(struct.pack("<4sIIII", b"glTF", 2, 28+len(header)+len(self.payload), len(header), 0x4E4F534A))
            f.write(header)
            f.write(struct.pack("<II", len(self.payload), 0x004E4942))
            f.write(self.payload)
        Path(f.name).replace(path)


def nearest(query, samples):
    # Small 3D blocks bound memory even for the full 48k-bud skeleton.
    return np.concatenate([np.argmin(np.sum((q[:, None] - samples)**2, axis=2), axis=1)
                           for q in np.array_split(query, max(1, len(query)//24))])


def branch_owners(points, triangles, travel, positions, paths, sprays):
    # UV seams duplicate vertices. Weld positions to recover each complete bark tube.
    unique, vertices = np.unique(points, axis=0, return_inverse=True)
    vertices = vertices.ravel()
    faces = vertices[triangles]
    labels = np.arange(len(unique))
    while True:
        previous = labels.copy()
        smallest = labels[faces].min(axis=1)
        for side in faces.T:
            np.minimum.at(labels, side, smallest)
        labels = labels[labels]
        if np.array_equal(previous, labels):
            break
    tubes = np.unique(labels[vertices], return_inverse=True)[1].ravel()
    max_travel = np.full(tubes.max()+1, -np.inf)
    np.maximum.at(max_travel, tubes, travel)
    tip = travel >= max_travel[tubes] - 1e-5
    counts = np.bincount(tubes[tip])
    ring_centres = np.stack([np.bincount(tubes[tip], weights=c)/counts for c in points[tip].T], axis=1)
    ends = positions[[path[-1] for path in paths]]
    tube_owner = nearest(ring_centres, ends)
    if len(np.unique(tube_owner)) != len(paths) or np.max(np.linalg.norm(ends[tube_owner]-ring_centres, axis=1)) > 0.04:
        raise ValueError("The bark and skeleton do not describe the same branches.")
    owner = np.zeros(len(positions), dtype=int)
    for i, path in enumerate(paths):
        owner[path[1:]] = i
    return tube_owner[tubes], owner[nearest(sprays, positions)]


def quat(axis, degrees):
    angle = np.deg2rad(degrees)*0.5
    return np.r_[axis / np.linalg.norm(axis) * np.sin(angle), np.cos(angle)]


def compose(q, r):
    u, v = q[..., :3], r[..., :3]
    return np.concatenate((q[..., 3:] * v + r[..., 3:] * u + np.cross(u, v),
                           q[..., 3:] * r[..., 3:] - (u*v).sum(axis=-1, keepdims=True)), axis=-1)


def rotate(q, points):
    t = 2*np.cross(q[..., :3], points)
    return points + q[..., 3:]*t + np.cross(q[..., :3], t)


def structure(skeleton, revision):
    paths = skeleton["primitives"]["points"]
    positions = np.array(skeleton["points"]["positions"])
    parents = skeleton["primitives"]["attributes"]["parents"]["values"]
    n = len(paths)
    rotation = np.tile([0., 0., 0., 1.], (n, 1))
    offset = np.zeros((n, 3))
    scale = np.ones(n)
    keep = np.ones(n, bool)
    rng = np.random.default_rng(7026)
    variation = rng.random((n, 3))
    up = np.array([0., 1., 0.])
    for i, chain in enumerate(parents):
        if not chain or revision == 0:
            continue
        parent = chain[-1]-1
        root = positions[paths[i][0]]
        tip = positions[paths[i][-1]]
        primary = (chain + [i+1])[1]
        featured = primary == 7
        depth = len(chain)
        lift, swing, length = 0., 0., 1.
        u, v, w = variation[i]
        retain = True
        if depth == 1 and featured:
            lift, swing, length = 9., -15., 1.09
        elif depth == 1 and revision >= 2 and np.linalg.norm(tip-root) > 1.0:
            lift = {3: 9, 4: -4, 5: 7, 8: 13, 9: 11, 10: 16, 11: 10, 13: -4, 14: -3, 15: 2}.get(primary, 5)
            swing, length = (v-0.5)*12., 0.91+0.13*w
            if revision >= 3 and root[1] > 7:
                # Shorter upper laterals and one taller leader break up the flat shoulder.
                length *= 0.83 if primary != 11 else 1.02
                lift += 14 if primary == 11 else 6
            if revision >= 4 and primary == 16:
                lift, length, swing = 27., 1.32, -8.
            elif revision >= 4 and primary in (9, 10, 11):
                length *= 0.90
                lift -= 4
        if depth == 2 and (featured or revision >= 2):
            limb = positions[paths[parent]]
            attach = np.argmin(np.linalg.norm(limb-root, axis=1))/(len(limb)-1)
            if featured:
                # Two separate windows along the featured scaffold; keep the far tip tufted.
                retain = not (0.19 < attach < 0.37 or 0.48 < attach < 0.62)
                swing, lift = (v-0.5)*30, (w-0.45)*24
            elif revision >= 2:
                threshold = 0.58 if attach < 0.63 else 0.24
                if revision >= 3 and 0.32 < attach < 0.65:
                    threshold += 0.13
                retain = u > threshold
                swing, lift = (v-0.5)*28, (w-0.45)*18
            if revision >= 4 and primary == 16:
                # A narrow leading shoot above two broader, lower shoulders.
                length = 0.65 + 0.19*w
                retain = u > 0.24 or attach > 0.85
            elif revision >= 4 and primary in (7, 9, 11) and attach > 0.85:
                # Carry one leafy extension beyond the exposed length of scaffold.
                retain = True
                length *= 0.84 + 0.22*v
        if depth == 3 and revision >= 2:
            threshold = 0.20 if featured else 0.25
            if revision >= 3:
                # Leave full tufts at some tips instead of thinning every tuft equally.
                threshold = 0.08 if variation[parent, 2] > 0.55 else 0.34
            retain = u > threshold
        radial = tip-root
        axis = np.cross(radial, up)
        local = quat(up, swing)
        if lift and np.linalg.norm(axis) > 1e-7:
            local = compose(local, quat(axis, lift))
        rotation[i] = compose(rotation[parent], local)
        scale[i] = scale[parent]*length
        # Rotate about the actual bud and carry descendants with the wood.
        anchor = offset[parent] + scale[parent]*rotate(rotation[parent], root)
        if revision >= 3 and depth == 1:
            anchor[1] += 0.30 * max(root[1]-2.5, 0.)
        offset[i] = anchor - scale[i]*rotate(rotation[i], root)
        keep[i] = keep[parent] and retain
    return rotation, offset, scale, keep


def taper_scaffolds(points, normals, owners, skeleton, revision):
    # Pull bark towards the local centreline. Retain the basal flare and the
    # original longitudinal coordinates of each vertex on the curved branch.
    points, normals = points.copy(), normals.copy()
    positions = np.array(skeleton["points"]["positions"])
    generations = skeleton["primitives"]["attributes"]["parents"]["values"]
    for i, path in enumerate(skeleton["primitives"]["points"]):
        if len(generations[i]) > 1:
            continue
        mask = owners == i
        centreline = positions[path]
        start = centreline[:-1]
        seg = np.diff(centreline, axis=0)
        length2 = np.maximum(np.sum(seg*seg, axis=1), 1e-10)
        local = points[mask, None] - start
        fraction = np.clip(np.sum(local*seg, axis=2)/length2, 0, 1)
        residual = local - fraction[..., None]*seg
        nearest_segment = np.argmin(np.sum(residual*residual, axis=2), axis=1)
        radial = residual[np.arange(mask.sum()), nearest_segment]
        tangent = seg[nearest_segment]/np.sqrt(length2[nearest_segment,None])
        height = points[mask, 1].copy()
        thickness = 1 - (0.29 if i == 0 else 0.17) * np.clip((height+0.25)/2.1, 0, 1)
        if revision >= 4 and i and height.max() > 2:
            progress = (nearest_segment + fraction[np.arange(mask.sum()),nearest_segment]) / len(seg)
            thickness *= 1.0 - 0.18*progress
        points[mask] -= radial*(1-thickness[:,None])
        n = normals[mask]
        axial = (n*tangent).sum(axis=1, keepdims=True)*tangent
        n = axial + (n-axial)/thickness[:,None]
        if i == 0:
            points[mask, 1] += 0.30*np.maximum(height-2.5, 0)
            n[:,1] /= np.where(height>2.5, 1.30, 1.)
        normals[mask] = n/np.maximum(np.linalg.norm(n,axis=1,keepdims=True),1e-9)
    return points, normals


def shape_terminal_fans(skeleton, rot, off, size, wood, foliage, bark, normals, sprouts, orientations):
    """Spread each secondary's descendants about their shared graft in world space."""
    chains = skeleton["primitives"]["attributes"]["parents"]["values"]
    paths = skeleton["primitives"]["points"]
    positions = np.array(skeleton["points"]["positions"])
    fans = np.array([(p+[i+1])[2]-1 if len(p) >= 2 else -1 for i,p in enumerate(chains)])
    scale = np.ones((len(paths),3))
    anchor = np.zeros_like(scale)
    rng = np.random.default_rng(315)
    for i in np.flatnonzero(fans == np.arange(len(paths))):
        graft = positions[paths[i][0]]
        anchor[i] = off[i]+size[i]*rotate(rot[i],graft)
        spread = rng.uniform(0.97,1.15)
        scale[i] = (spread,rng.uniform(0.48,0.84),spread)
    # The final neutral slot serves trunk and primary limbs (fan index -1).
    scale = np.vstack([scale,[1.,1.,1.]])
    anchor = np.vstack([anchor,[0.,0.,0.]])
    wood_fan, leaf_fan = fans[wood], fans[foliage]
    bark = anchor[wood_fan] + (bark-anchor[wood_fan])*scale[wood_fan]
    normals = normals/scale[wood_fan]
    normals /= np.linalg.norm(normals,axis=1,keepdims=True)
    sprouts = anchor[leaf_fan] + (sprouts-anchor[leaf_fan])*scale[leaf_fan]
    axis = rotate(orientations,np.array([0.,1.,0.]))
    bent = axis*scale[leaf_fan]
    bent /= np.linalg.norm(bent,axis=1,keepdims=True)
    correction = np.column_stack([np.cross(axis,bent),1.+np.sum(axis*bent,axis=1)])
    correction /= np.linalg.norm(correction,axis=1,keepdims=True)
    return bark,normals,sprouts,compose(correction,orientations)


def fixed_indices(path, level=3):
    # Same existing fixed template for every pose and revision in this model study.
    with path.open("rb") as f:
        if f.read(8) != b"FIXEDLOD":
            raise ValueError(path)
        f.seek(36)
        def count():
            return struct.unpack("<I", f.read(4))[0]
        def skip(stride):
            f.seek(count()*stride, 1)
        skip(220)
        skip(4)
        if count() == 7:
            skip(40)
        for i in range(level+1):
            raw = f.read(count()*4)
            if i == level:
                return np.frombuffer(raw, "<u4")
            skip(8)


def apply_preview_template(glb):
    assets = lighting.SCENE / "assets"
    for part, suffix in zip(glb.doc["meshes"][1]["primitives"], ("leaves", "spray-wood")):
        glb.replace(part["indices"], fixed_indices(assets / f"{NAME}.{suffix}.fixedlod"))


def author(scene, revision, export=False):
    assets = lighting.SCENE / "assets"
    glb = OakGlb(assets / (NAME+".glb"))
    trunk = glb.doc["meshes"][0]["primitives"]
    bark = trunk[0]
    attr = bark["attributes"]
    pos = glb.array(attr["POSITION"])
    normals = glb.array(attr["NORMAL"])
    triangles = glb.array(bark["indices"]).reshape(-1, 3)
    transforms = glb.doc["nodes"][1]["extensions"]["EXT_mesh_gpu_instancing"]["attributes"]
    origins = glb.array(transforms["TRANSLATION"])
    original_rot = glb.array(transforms["ROTATION"])
    skeleton = json.loads(SKELETON.read_text())
    wood, foliage = branch_owners(pos, triangles, glb.array(attr["TEXCOORD_2"])[:, 0],
                                  np.array(skeleton["points"]["positions"]), skeleton["primitives"]["points"], origins)
    rot, off, size, keep = structure(skeleton, revision)
    if revision >= 3:
        pos, normals = taper_scaffolds(pos, normals, wood, skeleton, revision)
    pos = rotate(rot[wood],pos)*size[wood,None]+off[wood]
    normals = rotate(rot[wood],normals)
    sprouts = rotate(rot[foliage],origins)*size[foliage,None]+off[foliage]
    orientations = compose(rot[foliage],original_rot)
    if revision >= 5:
        pos,normals,sprouts,orientations = shape_terminal_fans(
            skeleton,rot,off,size,wood,foliage,pos,normals,sprouts,orientations)
    used, remap = np.unique(triangles[keep[wood[triangles[:, 0]]]], return_inverse=True)
    for semantic, accessor in attr.items():
        values = glb.array(accessor)[used]
        if semantic == "POSITION":
            values = pos[used]
        elif semantic == "NORMAL":
            values = normals[used]
        glb.replace(accessor, values)
    glb.replace(bark["indices"], remap.ravel())
    valid = keep[foliage]
    glb.replace(transforms["TRANSLATION"], sprouts[valid])
    glb.replace(transforms["ROTATION"], orientations[valid])
    if export:
        MODEL.parent.mkdir(parents=True, exist_ok=True)
        glb.doc["scenes"][0]["name"] = "Open Fork White Oak"
        glb.write(MODEL)
    apply_preview_template(glb)
    glb.write(scene / "assets" / (NAME+".glb"))
    print(f"Revision {revision}: {keep.sum()}/{len(keep)} wood branches, {valid.sum()}/{len(valid)} sprays.", flush=True)


def stage(revision, rebuild, export=False):
    scene = OUT / f"r{revision}"
    assets = scene / "assets"
    assets.mkdir(parents=True, exist_ok=True)
    for src in [lighting.EXE, *(lighting.SCENE / "assets").glob("MeadowDaySky.*")]:
        target = (scene if src == lighting.EXE else assets) / src.name
        if not target.exists():
            shutil.copy2(src, target)
    target = assets / (NAME+".glb")
    if revision == 5 and MODEL.exists() and not (rebuild or export):
        if not target.exists() or MODEL.stat().st_mtime_ns > target.stat().st_mtime_ns:
            glb = OakGlb(MODEL)
            apply_preview_template(glb)
            glb.write(target)
    elif rebuild or export or not target.exists():
        author(scene, revision, export)
    return scene


def render(scene, pose, capture, trees):
    d, yaw, pitch = POSES[pose]
    args = ["--scene","forked-oak","--camera","overview","--width","3840","--height","2160","--msaa","1",
            "--tree-count",str(trees),"--distance",str(d),"--yaw-degrees",str(yaw),"--pitch-degrees",str(pitch),
            "--wind-strength","0","--wind-time","0","--probe-gi","bounce","--lighting","combined"]
    if pose == "rim":
        args += ["--focus-offset-z","5"]
    if pose == "backlight":
        args += ["--focus-offset-x","-1","--focus-offset-z","7"]
    env = os.environ.copy()
    env.pop(lighting.BASELINE_ENV, None)
    exe = scene / lighting.EXE.name
    if capture:
        capture.parent.mkdir(parents=True, exist_ok=True)
        subprocess.run([exe,"--capture",capture.resolve(),*args],cwd=scene,env=env,check=True,timeout=180)
    else:
        subprocess.Popen([exe,"--review-capture",*args],cwd=scene,env=env)


if __name__ == "__main__":
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--pose", choices=POSES, default="three-quarter")
    p.add_argument("--revision", type=int, choices=range(6), default=5)
    p.add_argument("--trees", type=int, choices=(1,4), default=4)
    p.add_argument("--capture", type=Path)
    p.add_argument("--rebuild", action="store_true")
    p.add_argument("--export", action="store_true", help="Save the final remodeled GLB at full source detail")
    opts = p.parse_args()
    lighting.prepare()
    if opts.export and opts.revision != 5:
        p.error("Only the final revision can be exported as WhiteOak-OpenFork.")
    render(stage(opts.revision, opts.rebuild, opts.export), opts.pose, opts.capture, opts.trees)
