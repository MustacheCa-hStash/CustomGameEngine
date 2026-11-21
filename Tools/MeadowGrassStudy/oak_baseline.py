"""Build and inspect the full-geometry grass benchmark in the open-crown oak viewer."""
import argparse
import collections
import copy
import filecmp
import itertools
import json
import math
import os
from pathlib import Path
import random
import shutil
import subprocess
import sys
import tempfile

import numpy as np

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "Tools/OakLightingStudy"))
import remodel as oak

OUT = ROOT / "build/meadow-oak-baseline"
STAGE = OUT / "scene"
ASSETS = STAGE / "assets"
MODEL = ASSETS / (oak.NAME + ".glb")
CONTROL = ASSETS / "GrassOnly.glb"
VIEWER = STAGE / "OakSprayLighting.exe"
CROWN = ROOT / "assets/source/white-oak-crown-study/WhiteOak-OpenCrown-Simplified.glb"
TUFT = ROOT / "assets/source/meadow-grass-study/meadow-tuft.glb"
# World-space target, orbit radius, yaw and pitch. Y is up.
VIEWS = {
    "blade": ((3.4323, 0.35335, 4.39624), 0.25, 18.5, 12.0),
    "edge": ((-1.4, 0.45, 2.0), 5.45, 67, 13),
    "sun": ((3.35, 0.42, 2.65), 2.5, -28, 19),
    "roots": ((-3.03, 0.12, 4.88), 1.55, -8, 24),
    "layout": ((-1.9, 6.5, 1.0), 31, 9, 12),
}


def buffer_view(model, raw):
    model.payload.extend(bytes(-len(model.payload) % 4))
    views = model.doc["bufferViews"]
    views.append(dict(buffer=0, byteOffset=len(model.payload), byteLength=len(raw)))
    model.payload.extend(raw)
    return len(views)-1


def accessor(model, values):
    values = np.ascontiguousarray(values)
    component = {"f": 5126, "u": 5125}[values.dtype.kind]
    assert values.dtype.itemsize == 4
    acc = dict(bufferView=buffer_view(model, values.tobytes()), componentType=component,
               count=len(values), type={1:"SCALAR",2:"VEC2",3:"VEC3",4:"VEC4"}[values.shape[1]],
               min=values.min(axis=0).tolist(),max=values.max(axis=0).tolist())
    model.doc["accessors"].append(acc)
    return len(model.doc["accessors"])-1


def add_materials(model, grass):
    d, g = model.doc, grass.doc
    image_offset, sampler_offset, tex_offset = (len(d[k]) for k in ("images","samplers","textures"))
    for image in g["images"]:
        view = g["bufferViews"][image["bufferView"]]
        offset = view.get("byteOffset",0)
        raw = grass.payload[offset:offset+view["byteLength"]]
        d["images"].append(dict(image, bufferView=buffer_view(model,raw)))
    d["samplers"].extend(g["samplers"])
    for tex in g["textures"]:
        d["textures"].append(dict(tex,source=tex["source"]+image_offset,sampler=tex["sampler"]+sampler_offset))
    for mat in copy.deepcopy(g["materials"]):
        for ref in (mat.get("normalTexture"),mat["pbrMetallicRoughness"].get("baseColorTexture")):
            if ref:
                ref["index"] += tex_offset
        transmission = mat.get("extensions",{}).get("KHR_materials_diffuse_transmission")
        if transmission is not None:
            # The Blender Translucent BSDF is driven by the same tissue map as the front.
            transmission["diffuseTransmissionColorTexture"] = dict(mat["pbrMetallicRoughness"]["baseColorTexture"])
        d["materials"].append(mat)


def tuft_mesh(model, grass, offset):
    # Join by material without changing any triangle, UV, or vertex normal.
    groups = collections.defaultdict(list)
    for node in grass.doc["nodes"]:
        if set(node)-{"name","mesh"}:
            raise ValueError("Expected the study tuft's identity node transforms")
        for prim in grass.doc["meshes"][node["mesh"]]["primitives"]:
            groups[prim["material"]].append(prim)
    joined, count = [], 0
    for mat, parts in groups.items():
        streams = collections.defaultdict(list)
        indices, base = [], 0
        for p in parts:
            indices.append(grass.array(p["indices"]).astype("<u4")+base)
            for key, index in p["attributes"].items():
                streams[key].append(grass.array(index))
            base += len(streams["POSITION"][-1])
        faces = np.concatenate(indices)
        count += len(faces)//3
        joined.append(dict(indices=accessor(model,faces), material=mat+offset, attributes={
            key:accessor(model,np.concatenate(arrays)) for key,arrays in streams.items()}))
    model.doc["meshes"].append(dict(name="Full meadow tuft",primitives=joined))
    return len(model.doc["meshes"])-1, count


def scatter():
    rng = random.Random(920415)
    positions, rotations, scales = [], [], []
    for _ in range(30000):
        x, z = rng.uniform(-9.7,5.6), rng.uniform(-0.55,4.91)
        if ((x+2.05)/7.65)**2 + ((z-2.18)/2.73)**2 > 1 or math.hypot(x,z)<0.72:
            continue
        if any((x-px)**2+(z-pz)**2 < 0.27**2 for px,_,pz in positions):
            continue
        half = rng.random()*math.pi
        scale = rng.uniform(0.79,1.08)
        positions.append((x,-0.012,z))
        rotations.append((0,math.sin(half),0,math.cos(half)))
        scales.append((scale,)*3)
        if len(positions)==389:
            break
    else:
        raise ValueError("Could not fit the reference tuft count into the strip")
    return dict(TRANSLATION=positions,ROTATION=rotations,SCALE=scales)


def world_center(model):
    bounds = []
    for node in model.doc["nodes"]:
        attrs = node.get("extensions",{}).get("EXT_mesh_gpu_instancing",{}).get("attributes")
        for p in model.doc["meshes"][node["mesh"]]["primitives"]:
            a = model.doc["accessors"][p["attributes"]["POSITION"]]
            corners = np.array(list(itertools.product(*zip(a["min"],a["max"]))))
            if attrs:
                scales = model.array(attrs["SCALE"])[:,None,:] if "SCALE" in attrs else 1
                corners = oak.rotate(model.array(attrs["ROTATION"])[:,None,:],corners*scales)
                corners = (model.array(attrs["TRANSLATION"])[:,None,:]+corners).reshape(-1,3)
            bounds.extend([corners.min(axis=0),corners.max(axis=0)])
    return (np.min(bounds,axis=0)+np.max(bounds,axis=0))/2


def stage():
    source = ROOT / "build/oak-crown-optimized/scene"
    if not (source / VIEWER.name).is_file() or not CROWN.is_file():
        subprocess.run(["powershell","-NoProfile","-ExecutionPolicy","Bypass","-File",
                        ROOT/"Tools/BuildOakUpperCrownLod.ps1"],cwd=ROOT,check=True)
    ASSETS.mkdir(parents=True,exist_ok=True)
    for src in [source/VIEWER.name, *(source/"assets").glob("MeadowDaySky.*")]:
        dest = (STAGE if src.suffix==".exe" else ASSETS)/src.name
        if not dest.exists() or not filecmp.cmp(src,dest,shallow=False):
            shutil.copy2(src,dest)
    manifest = OUT/"baseline.json"
    if MODEL.exists() and CONTROL.exists() and manifest.exists() and MODEL.stat().st_mtime >= max(
            p.stat().st_mtime for p in (CROWN,TUFT,Path(__file__))):
        return json.loads(manifest.read_text())
    grass = oak.OakGlb(TUFT)
    trs = scatter()
    metadata = dict(tufts=len(trs["SCALE"]))
    for dest in (MODEL,CONTROL):
        model = oak.OakGlb(CROWN)
        if dest == CONTROL:
            model.payload.clear()
            for key in ("nodes","meshes","materials","textures","images","samplers","bufferViews","accessors"):
                model.doc[key] = []
            model.doc["scenes"] = [{"nodes":[]}]
        offset = len(model.doc["materials"])
        add_materials(model,grass)
        mesh, triangles = tuft_mesh(model,grass,offset)
        attrs = {k:accessor(model,np.array(v,dtype="<f4")) for k,v in trs.items()}
        node = dict(name="Full grass strip",mesh=mesh,extensions={"EXT_mesh_gpu_instancing":{"attributes":attrs}})
        model.doc["scenes"][0]["nodes"].append(len(model.doc["nodes"]))
        model.doc["scenes"][0]["name"] = "One oak and grass" if dest==MODEL else "Open-sky grass control"
        model.doc["nodes"].append(node)
        model.payload.extend(bytes(-len(model.payload)%4))
        model.doc["buffers"][0]["byteLength"] = len(model.payload)
        model.write(dest)
        metadata["controlFocus" if dest==CONTROL else "focus"] = world_center(model).tolist()
        metadata["trianglesPerTuft"] = triangles
    manifest.write_text(json.dumps(metadata,indent=2))
    return metadata


def run(pose, focus, mode, lighting, control, *, viewer=VIEWER, output_dir=OUT, environment=None, extra_args=()):
    target, dist, yaw, pitch = VIEWS[pose]
    args = ["--scene",str(CONTROL) if control else "forked-oak","--camera","overview","--distance",str(dist),"--yaw-degrees",str(yaw),
            "--pitch-degrees",str(pitch),"--msaa","4","--width","3840","--height","2160",
            "--sun-azimuth","102","--sun-elevation","49","--exposure-ev","0","--wind-strength","0",
            "--wind-time","0","--probe-gi","bounce","--lighting",lighting]
    for axis,value in zip("xyz",np.array(target)-focus):
        args.extend(["--focus-offset-"+axis,str(value)])
    args.extend(extra_args)
    env = (os.environ if environment is None else environment).copy()
    env.pop(oak.lighting.BASELINE_ENV,None)
    if mode=="view":
        subprocess.Popen([viewer,"--review-capture",*args],cwd=STAGE,env=env)
    else:
        base = pose if lighting=="combined" else pose+"-"+lighting
        if control:
            base += "-open"
        output_dir.mkdir(parents=True,exist_ok=True)
        with tempfile.TemporaryDirectory(dir=output_dir) as tmp:
            output = Path(tmp)/(base+".png")
            subprocess.run([viewer,"--capture",output,*args],cwd=STAGE,env=env,check=True,timeout=240)
            output.replace(output_dir/output.name)
            timing = output.with_suffix(".timings.csv")
            if timing.exists():
                timing.replace(output_dir/timing.name)


if __name__=="__main__":
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("mode",choices=("stage","capture","view"),default="capture",nargs="?")
    ap.add_argument("--pose",choices=VIEWS)
    ap.add_argument("--lighting",choices=("combined","shadow-visibility"),default="combined")
    ap.add_argument("--open",action="store_true",help="Use the identical grass layout without the oak")
    args = ap.parse_args()
    metadata = stage()
    print(metadata,flush=True)
    if args.mode != "stage":
        for pose in ([args.pose] if args.pose else ["edge"] if args.mode=="view" else VIEWS):
            run(pose,metadata["controlFocus" if args.open else "focus"],args.mode,args.lighting,args.open)
