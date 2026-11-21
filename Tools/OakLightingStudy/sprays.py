"""Four modeled white-oak shoot forms for the open-fork lighting study."""
import argparse
import copy
import filecmp
import itertools
import os
import shutil
import subprocess
import tempfile
from pathlib import Path

import numpy as np

import remodel as oak


OUT = oak.lighting.ROOT / "build/oak-spray-study"
MODEL = oak.MODEL.with_name("WhiteOak-VariedSprays.glb")
FORMS = ("Fan", "Drooping", "Extension", "Tuft")
SPRAY_EXE = "OakSprayLighting.exe"
# Distances and targets are in world metres. Both sides orbit the same outer bough.
VIEWS = {
    "crown": (28, 197, -4, (0, 9, 0)),
    "fork": (24, 132, -5.46, (0, 9, 0)),
    "lit": (7.6, 104, 12, (4.82, 12.53, 2.26)),
    "sunward": (7.6, -76, -36, (4.82, 12.53, 2.26)),
    "edge": (3.2, -96, -38, (5.48, 13.22, 2.7)),
    "grove": (41, 132, -5.46, (0, 9, 0)),
}


def unit(v):
    return v / np.maximum(np.linalg.norm(v, axis=-1, keepdims=True), 1e-10)


def add_array(g, values, like):
    example = copy.deepcopy(g.doc["accessors"][like])
    values = np.ascontiguousarray(values, dtype=g.array(like).dtype)
    g.payload.extend(b"\x00" * (-len(g.payload) % 4))
    view = {"buffer": 0, "byteOffset": len(g.payload), "byteLength": values.nbytes}
    target = g.doc["bufferViews"][example["bufferView"]].get("target")
    if target:
        view["target"] = target
    g.payload.extend(values.tobytes())
    example.update(bufferView=len(g.doc["bufferViews"]), count=len(values))
    example.pop("byteOffset", None)
    if "min" in example:
        example.update(min=values.min(axis=0).tolist(), max=values.max(axis=0).tolist())
    g.doc["bufferViews"].append(view)
    g.doc["accessors"].append(example)
    return len(g.doc["accessors"])-1


def choose_forms(p, q):
    # Compare a graft with the local foliage, so the upper surface of a lower
    # limb can carry fans too. Nearby tips tend to receive related shoot forms.
    delta = p[None,:,:] - p[:,None,:]
    weights = np.exp(-np.sum(delta**2, axis=2)/(2*1.05**2))
    crowd = weights.sum(axis=1)
    height = -np.sum(delta[:,:,1]*weights, axis=1)/crowd
    direction = oak.rotate(q, np.array([0.,1.,0.]))
    outward = np.sum(unit(p[:,[0,2]])*direction[:,[0,2]], axis=1)
    rng = np.random.default_rng(26031)
    noise = rng.normal(0, 0.09, len(p))
    forms = np.full(len(p), 3)
    def take(form, score, fraction):
        available = np.flatnonzero(forms == 3)
        chosen = available[np.argsort(score[available])[-round(len(p)*fraction):]]
        forms[chosen] = form
    take(2, outward - 0.6*np.log(crowd) + noise, 0.14)
    take(1, -height - 0.40*direction[:,1] + noise, 0.24)
    take(0, height + 0.38*outward + noise, 0.35)

    # Roll around the existing wood axis, keeping each graft rooted. Use world
    # up for the fan plane, with residual roll to break up parallel highlights.
    x = oak.rotate(q, np.array([1.,0.,0.]))
    z = oak.rotate(q, np.array([0.,0.,1.]))
    angle = np.arctan2(x[:,1], z[:,1]) * np.choose(forms, [0.65,0.29,0.27,0.06])
    angle += rng.normal(0, np.deg2rad(9), len(q))
    roll = np.zeros_like(q)
    roll[:,1], roll[:,3] = np.sin(angle/2), np.cos(angle/2)
    return forms, unit(oak.compose(q,roll))


def warp(v, params):
    width, reach, depth, bend, flare = params
    x,y,z = v.T
    return np.column_stack((width*x,reach*y,depth*z + bend*y*y + flare*x*x))


def make_shoot(g, source, form, middle):
    # Z is the upper side of the authored fan, Y grows out from its graft.
    params = [(1.44,0.98,0.72,-0.09,0.15),(1.04,1.06,1.08,-0.80,-0.50),
              (0.75,1.40,1.02,-0.13,0.12),(1.04,0.89,1.34,0.43,-0.30)][form]
    mesh = copy.deepcopy(source)
    mesh["name"] = "White oak " + FORMS[form].lower() + " spray"
    leaf, twig = mesh["primitives"]
    w = twig["attributes"]
    p,n,t = [g.array(w[key]) for key in ("POSITION","NORMAL","TANGENT")]
    sx,sy,sz,bend,flare = params
    dx,dy = 2*flare*p[:,0], 2*bend*p[:,1]
    nz = n[:,2]/sz
    normal = unit(np.column_stack(((n[:,0]-dx*nz)/sx,(n[:,1]-dy*nz)/sy,nz)))
    tangent = unit(np.column_stack((sx*t[:,0],sy*t[:,1],dx*t[:,0]+dy*t[:,1]+sz*t[:,2])))
    for key, val in (("POSITION",warp(p,params)),("NORMAL",normal),("TANGENT",np.column_stack([tangent,t[:,3]]))):
        w[key] = add_array(g,val,w[key])

    # Blades in this source are separate equal-sized vertex blocks. The high-V
    # end of their shared atlas is the petiole, within 1 mm of the twig.
    attr = leaf["attributes"]
    xyz = g.array(attr["POSITION"]).reshape(45,-1,3)
    uv = g.array(attr["TEXCOORD_0"]).reshape(45,-1,2)
    indices = g.array(leaf["indices"]).reshape(-1,3)
    blade = indices//xyz.shape[1]
    if np.any(np.ptp(blade,axis=1)):
        raise ValueError("Leaf layout differs from the open-fork source.")
    root_mask = uv[:,:,1] > uv[:,:,1].max(axis=1, keepdims=True)-0.001
    base = np.sum(xyz*root_mask[:,:,None],axis=1)/root_mask.sum(axis=1,keepdims=True)
    centered = xyz - xyz.mean(axis=1,keepdims=True)
    _, basis = np.linalg.eigh(centered.transpose(0,2,1) @ centered)
    blade_n = basis[:,:,0]
    blade_n *= np.where(blade_n[:,2:]>=0,1.,-1.)
    rng = np.random.default_rng(6100+form)
    # Inclinations follow the curvature of the shoot. Keep the individual
    # leaf's cupping, veins and lobes by rotating it rigidly at its petiole.
    desired = unit(np.column_stack((-2*flare*base[:,0]/sx, -2*bend*base[:,1]/sy, np.ones(len(base)))) +
                   rng.normal(0,[0.23,0.21,0.04],base.shape))
    strength = [0.64,0.40,0.31,0.22][form]
    target = unit((1-strength)*blade_n + strength*desired)
    swivel = unit(np.column_stack((np.cross(blade_n,target),1+np.sum(blade_n*target,axis=1))))[:,None,:]
    gpos = warp(base,params)[:,None,:] + oak.rotate(swivel,xyz-base[:,None,:])
    normals = oak.rotate(swivel,g.array(attr["NORMAL"]).reshape(xyz.shape))
    tangents = g.array(attr["TANGENT"]).reshape(45,-1,4)
    tangents[:,:,:3] = oak.rotate(swivel,tangents[:,:,:3])
    for key,val in (("POSITION",gpos),("NORMAL",normals),("TANGENT",tangents)):
        attr[key] = add_array(g,val.reshape(-1,val.shape[-1]),attr[key])
    kept = np.ones(45,bool)
    if form == 2:
        # Open two intervals along the lateral shoots, preserving tip leaves.
        distance = np.linalg.norm(base[:,:2],axis=1)
        gap = np.abs(distance-0.52) + np.where(base[:,0] > 0,0.035,0)
        kept[np.argsort(gap)[:14]] = False
    for part, is_leaf in ((leaf,True),(twig,False)):
        src_idx = part["indices"]
        raw = middle[is_leaf] if middle else g.array(src_idx)
        tri = raw.reshape(-1,3)
        if is_leaf:
            tri = tri[kept[tri[:,0]//xyz.shape[1]]]
        if middle or (is_leaf and form == 2):
            part["indices"] = add_array(g,tri.reshape(-1,1),src_idx)
    return mesh


def author(output, preview):
    g = oak.OakGlb(oak.MODEL)
    source = g.doc["meshes"].pop()
    node = g.doc["nodes"].pop()
    inst = node["extensions"]["EXT_mesh_gpu_instancing"]["attributes"]
    p,q = g.array(inst["TRANSLATION"]),g.array(inst["ROTATION"])
    forms, oriented = choose_forms(p,q)
    middle = None
    if preview:
        middle = {is_leaf: oak.fixed_indices(oak.lighting.SCENE/"assets"/f"{oak.NAME}.{suffix}.fixedlod")
                  for is_leaf,suffix in ((True,"leaves"),(False,"spray-wood"))}
    for form in range(4):
        mesh = make_shoot(g,source,form,middle)
        mesh_id = len(g.doc["meshes"])
        g.doc["meshes"].append(mesh)
        mask = forms == form
        attr = {"TRANSLATION":add_array(g,p[mask],inst["TRANSLATION"]),
                "ROTATION":add_array(g,oriented[mask],inst["ROTATION"])}
        g.doc["nodes"].append({"name":mesh["name"],"mesh":mesh_id,
                               "extensions":{"EXT_mesh_gpu_instancing":{"attributes":attr}}})
    g.doc["scenes"][0].update(name="White oak with varied sprays",nodes=list(range(len(g.doc["nodes"]))))
    g.doc["buffers"][0]["byteLength"] = len(g.payload)
    output.parent.mkdir(parents=True,exist_ok=True)
    g.write(output)
    print(dict(zip(FORMS,map(int,np.bincount(forms)))),flush=True)


def focus(scene):
    g = oak.OakGlb(scene/"assets"/(oak.NAME+".glb"))
    bounds = []
    for node in g.doc["nodes"]:
        attrs = node.get("extensions",{}).get("EXT_mesh_gpu_instancing",{}).get("attributes")
        for prim in g.doc["meshes"][node["mesh"]]["primitives"]:
            xyz = g.array(prim["attributes"]["POSITION"])
            lo,hi = xyz.min(axis=0),xyz.max(axis=0)
            if attrs:
                p,q = g.array(attrs["TRANSLATION"]),g.array(attrs["ROTATION"])
                corners = np.array(list(itertools.product(*zip(lo,hi))))
                xyz = p[:,None,:] + oak.rotate(q[:,None,:],corners)
                lo,hi = xyz.min(axis=(0,1)),xyz.max(axis=(0,1))
            bounds.extend([lo,hi])
    return (np.min(bounds,axis=0)+np.max(bounds,axis=0))/2


def prepare(rebuild=False):
    scene = OUT/"scene"
    folder = scene/"assets"
    folder.mkdir(parents=True,exist_ok=True)
    for src in [oak.lighting.EXE,*(oak.lighting.SCENE/"assets").glob("MeadowDaySky.*")]:
        dest=(scene if src==oak.lighting.EXE else folder)/src.name
        if not dest.exists():
            shutil.copy2(src,dest)
    asset=folder/(oak.NAME+".glb")
    inputs = [Path(__file__), oak.MODEL, *(oak.lighting.SCENE/"assets").glob(oak.NAME+".*.fixedlod")]
    if rebuild or not asset.exists() or any(f.stat().st_mtime_ns > asset.stat().st_mtime_ns for f in inputs):
        author(asset,True)
    return scene


def prepare_transmission(scene):
    # Use the frozen lighting study's renderer and change only direct leaf transmission.
    original = (oak.lighting.SOURCE/"shaders/OakStudy.hlsli").read_text()
    marker = "    const float direct = 0.65 * visibility;"
    term = "(lerp(1.0, phase, 0.80) - 1.0)"
    if original.count(marker) != 1 or original.count(term) != 1:
        raise ValueError("The baseline transmission shader differs from the expected lighting study.")
    shader = original.replace(marker,
        "    // Reduce off-axis transmission while retaining the forward peak.\n"
        "    const float alignment = dot(-v, sun);\n"
        "    const float angularWeight = lerp(0.60, 1.0, smoothstep(0.80, 0.995, alignment));\n" + marker)
    shader = shader.replace(term, "(lerp(1.0, phase, 0.80) * angularWeight - 1.0)")
    source = OUT/"transmission-source"
    if not source.exists():
        with tempfile.TemporaryDirectory(dir=OUT) as tmp:
            stage = Path(tmp)/"source"
            shutil.copytree(oak.lighting.SOURCE,stage)
            stage.rename(source)
    include = source/"shaders/OakStudy.hlsli"
    if include.read_text() != shader:
        include.write_text(shader)
    compiler = OUT/"transmission-build"
    binary = compiler/"Release"/oak.lighting.EXE.name
    if not binary.exists() or include.stat().st_mtime_ns > binary.stat().st_mtime_ns:
        cache = (oak.lighting.ROOT/"build/CMakeCache.txt").read_text().splitlines()
        cmake = next(line.split("=",1)[1] for line in cache if line.startswith("CMAKE_COMMAND:INTERNAL="))
        subprocess.run([cmake,"-S",source,"-B",compiler,"-G","Visual Studio 17 2022","-A","x64"],check=True)
        subprocess.run([cmake,"--build",compiler,"--config","Release","--target","CustomGameEngine","--parallel","6"],check=True)
    dest = scene/SPRAY_EXE
    if not dest.exists() or not filecmp.cmp(binary,dest,shallow=False):
        shutil.copy2(binary,dest)
    return dest


def render(scene, pose, capture, trees=1, extra=(), exe=None):
    distance,yaw,pitch,target=VIEWS[pose]
    offset=np.array(target)-focus(scene)
    args=["--scene","forked-oak","--camera","overview","--width","3840","--height","2160","--msaa","1",
          "--tree-count",str(trees),"--distance",str(distance),"--yaw-degrees",str(yaw),"--pitch-degrees",str(pitch),
          "--wind-strength","0","--wind-time","0","--probe-gi","bounce","--lighting","combined"]
    for axis,value in zip("xyz",offset):
        args.extend([f"--focus-offset-{axis}",str(value)])
    args.extend(extra)
    env=os.environ.copy()
    env.pop(oak.lighting.BASELINE_ENV,None)
    exe=exe or scene/oak.lighting.EXE.name
    if capture:
        capture.parent.mkdir(parents=True,exist_ok=True)
        subprocess.run([exe,"--capture",capture.resolve(),*args],cwd=scene,env=env,check=True,timeout=180)
    else:
        subprocess.Popen([exe,"--review-capture",*args],cwd=scene,env=env)


if __name__=="__main__":
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument("--pose",choices=VIEWS,default="fork")
    p.add_argument("--trees",type=int,choices=(1,4),default=1)
    lighting_group = p.add_mutually_exclusive_group()
    lighting_group.add_argument("--control",action="store_true",help="Previous crown and previous lighting")
    lighting_group.add_argument("--prior-lighting",action="store_true",help="New sprays with the previous lighting")
    p.add_argument("--rebuild",action="store_true")
    p.add_argument("--export",action="store_true")
    p.add_argument("--capture",type=Path)
    opts=p.parse_args()
    if not oak.lighting.EXE.exists():
        oak.lighting.prepare()
    if opts.export:
        author(MODEL,False)
    scene=oak.stage(5,False) if opts.control else prepare(opts.rebuild)
    exe = None if opts.control or opts.prior_lighting else prepare_transmission(scene)
    render(scene,opts.pose,opts.capture,opts.trees,exe=exe)
