"""Four-oak meadow and matched standing-height cameras for the lighting study."""
import argparse
import copy
import itertools
import json
import math
import os
from pathlib import Path
import shutil
import subprocess
import sys

import numpy as np
from PIL import Image

import coverage as grass
import oak_baseline as base


OUT = base.ROOT / "build/meadow-four-oaks"
ASSETS = OUT / "scene/assets"
VIEWER = ASSETS.parent / "FourOaks.exe"
EYE_HEIGHT = 1.65
CAMERAS = {
    "canopy": ((-23.2, -15.0), (-9.0, -16.3), 5.0),
    "clearing": ((-23.2, -15.0), (-30.0, -10.0), 5.0),
    "downhill": ((-34.0, -25.0), (-9.0, -9.5), 6.0),
}


def height(x, z):
    # Low shoulders frame a shallow, diagonal hollow through the stand.
    east = 2.65 * np.exp(-((x - 17 - 2*np.sin(z/9))/12)**2 - ((z + 5)/37)**2)
    north = 2.70 * np.exp(-((z + 33 + 3*np.sin(x/11))/14)**2 - ((x + 13)/42)**2)
    hollow = -0.48 * np.exp(-((x + 5 - 0.25*z)/5.5)**2 - ((z + 4)/20)**2)
    west = 2.45 * np.exp(-((x+38+3*np.sin(z/13))/13)**2 - ((z+7)/31)**2)
    south = 1.65 * np.exp(-((z-16+2*np.sin(x/10))/12)**2 - ((x+10)/37)**2)
    return east + north + west + south + hollow + 0.21*np.sin((x+z)/8) + 0.13*np.cos((z-x)/5.5)


def terrain():
    axis = np.arange(-88, 88.5, 0.5)
    x, z = np.meshgrid(axis, axis)
    h = height(x, z)
    dz, dx = np.gradient(h, 0.5)
    normal = np.stack((-dx, np.ones_like(x), -dz), axis=-1)
    normal /= np.linalg.norm(normal, axis=-1, keepdims=True)
    first = np.arange(x.size).reshape(x.shape)[:-1, :-1].ravel()
    side = len(axis)
    faces = np.stack((first, first+side, first+side+1, first, first+side+1, first+1), axis=1)
    grid = base.oak.OakGlb(grass.SHORT)
    grid.doc = dict(asset={"version":"2.0"}, scenes=[dict(nodes=[0])], scene=0,
                    nodes=[dict(mesh=0,name="Meadow terrain")], bufferViews=[], accessors=[])
    grid.payload = bytearray()
    streams = dict(POSITION=np.stack((x,h,z),axis=-1).reshape(-1,3), NORMAL=normal.reshape(-1,3),
                   TEXCOORD_0=np.stack((x,z),axis=-1).reshape(-1,2))
    prim = grass.encode_part(grid, streams, faces, 0)
    del prim["material"]
    grid.doc["meshes"] = [dict(primitives=[prim])]
    grass.write(grid, ASSETS / "MeadowTerrain.glb")


def node_bounds(model, node):
    bounds = []
    attrs = node.get("extensions",{}).get("EXT_mesh_gpu_instancing",{}).get("attributes")
    for prim in model.doc["meshes"][node["mesh"]]["primitives"]:
        pos = prim["attributes"]["POSITION"]
        if attrs:
            meta = model.doc["accessors"][pos]
            box = list(itertools.product(*zip(meta["min"],meta["max"])))
            scale = model.array(attrs["SCALE"])[:,None,:] if "SCALE" in attrs else 1
            xyz = base.oak.rotate(model.array(attrs["ROTATION"])[:,None,:], np.array(box)*scale)
            xyz = (xyz + model.array(attrs["TRANSLATION"])[:,None,:]).reshape(-1,3)
        else:
            xyz = model.array(pos)
        if "rotation" in node:
            xyz = base.oak.rotate(np.array(node["rotation"]),xyz*np.array(node["scale"])) + node["translation"]
        bounds.extend((xyz.min(axis=0),xyz.max(axis=0)))
    return bounds


def scene_oaks(model):
    originals = [node for node in model.doc["nodes"] if node["name"] not in ("Full grass strip","Short grass")]
    bounds = np.array([p for n in originals for p in node_bounds(model,n)])
    spacing = max(np.ptp(bounds,axis=0)[[0,2]]) + 2
    trees = []
    expanded = []
    # Match ViewerScene::setForkedOakCount(4) before adding any grass or terrain.
    for i in range(4):
        x, z = ((i%2-1)*spacing, (i//2-1)*spacing)
        yaw = 0 if i==3 else 1.25*i
        size = 1 if i==3 else 0.92 + 0.04*((7*i)%5)
        trees.append((x,z))
        for src in originals:
            node = copy.deepcopy(src)
            node.update(translation=[x,float(height(x,z))-0.12*size,z], rotation=[0,math.sin(yaw/2),0,math.cos(yaw/2)],
                        scale=[size]*3)
            expanded.append(node)
    return trees, expanded


def scatter(trees, short, seed=None):
    rng = np.random.default_rng(seed if seed is not None else (73104 if short else 41209))
    step = 0.28 if short else 0.44
    gx,gz = np.meshgrid(np.arange(-47,31,step),np.arange(-47,31,step))
    x = gx.ravel()+rng.uniform(-0.45,0.45,gx.size)*step
    z = gz.ravel()+rng.uniform(-0.45,0.45,gz.size)*step
    patch = 0.50 + 0.19*np.sin(x*0.73+z*0.36) + 0.16*np.cos(z*0.84-x*0.48) + 0.13*np.sin(x*0.24-z*0.39)
    nearest = np.min([np.hypot(x-tx,z-tz) for tx,tz in trees],axis=0)
    root = np.clip((nearest-(0.76 if short else 1.05))/2.6,0,1)
    shade = np.max([np.exp(-((x-(tx-6.3))/6.8)**2-((z-(tz+1.4))/6.5)**2) for tx,tz in trees],axis=0)
    radius = np.sqrt(((x+9)/38)**2+((z+9)/38)**2)
    border = np.clip((1-radius)*8,0,1)
    growth = (0.38+0.55*patch if short else 0.55+0.40*patch)*(1-(0.10 if short else 0.44)*shade)
    chosen = rng.random(len(x)) < growth*root*border
    x,z,patch,shade = (v[chosen] for v in (x,z,patch,shade))
    size = rng.uniform(0.78,1.22,len(x))*(0.93+0.25*patch)*(1-0.12*shade)
    width = size*(1.65 if short else 1.17)
    length = size*(1.40 if short else 1.0)
    angle = rng.uniform(0,math.tau,len(x))
    zero = np.zeros_like(x)
    return dict(TRANSLATION=np.column_stack((x,height(x,z)-(0.006 if short else 0.014),z)),
                ROTATION=np.column_stack((zero,np.sin(angle/2),zero,np.cos(angle/2))),
                SCALE=np.column_stack((width,length,width)))


def visibility(layers, asset_dir=ASSETS):
    kernels = OUT / "tuft-kernels.png"
    if not kernels.exists():
        items = []
        for x,asset in ((-1,grass.LOW),(1,grass.SHORT)):
            items.append(dict(asset=str(asset),placements=dict(TRANSLATION=[[x,-0.01 if x<0 else -0.006,0]],
                                                              ROTATION=[[0,0,0,1]],SCALE=[[1,1,1]])))
        manifest = OUT / "tuft-kernels.json"
        manifest.write_text(json.dumps(dict(layers=items,camera=dict(x=0,z=0,width=4,resolution=[384,192]))))
        blender = shutil.which("blender") or Path(os.environ["ProgramFiles"])/"Blender Foundation/Blender 5.0/blender.exe"
        subprocess.run([blender,"-b","--python-exit-code","1","-P",grass.HERE/"bake_ground_visibility.py",
                        "--",manifest,kernels],check=True)
    atlas = Image.open(kernels).convert("L")
    pixels_per_metre = 48
    field = np.zeros((80*pixels_per_metre,80*pixels_per_metre),np.float32)
    for layer,trs in enumerate(layers):
        tile = atlas.crop((192*layer,0,192*(layer+1),192))
        stamps = {}
        for (x,_,z),(_,qy,_,qw),(scale,_,_) in zip(*[trs[k] for k in ("TRANSLATION","ROTATION","SCALE")]):
            diameter = max(4,2*round(pixels_per_metre*scale))
            rotation = round(math.degrees(2*math.atan2(qy,qw))/15)%24
            key = (diameter,rotation)
            if key not in stamps:
                image = tile.rotate(15*rotation,Image.Resampling.BILINEAR,fillcolor=255).resize((diameter,diameter),Image.Resampling.LANCZOS)
                stamps[key] = -np.log(np.maximum(1,np.asarray(image,dtype=np.float32))/255)
            px,pz = round((x+48)*pixels_per_metre)-diameter//2,round((z+48)*pixels_per_metre)-diameter//2
            x0,z0 = max(0,px),max(0,pz)
            x1,z1 = min(field.shape[1],px+diameter),min(field.shape[0],pz+diameter)
            if x0 < x1 and z0 < z1:
                field[z0:z1,x0:x1] += stamps[key][z0-pz:z1-pz,x0-px:x1-px]
    Image.fromarray(np.rint(255*np.exp(-field)).astype("uint8")).save(asset_dir/"MeadowGroundVisibility.png")


def prepare():
    ASSETS.mkdir(parents=True,exist_ok=True)
    control = grass.OUT/"optimized/scene/assets"
    if not (control/base.MODEL.name).exists():
        subprocess.run([sys.executable,"-B",grass.HERE/"coverage.py","prepare","--arm","optimized"],check=True)
    grass.lighting.prepare_viewer(upstream=grass.OUT/"source",out=OUT,patch=grass.HERE/"four_oaks.patch",destination=VIEWER)
    for path in [control/"MeadowSoil.png", *control.glob("MeadowDaySky.*")]:
        grass.copy_file(path, ASSETS/path.name)
    terrain()
    model = base.oak.OakGlb(control/base.MODEL.name)
    trees,nodes = scene_oaks(model)
    layers = []
    for short,name in enumerate(("Full grass strip","Short grass")):
        node = next(n for n in model.doc["nodes"] if n["name"]==name)
        trs = scatter(trees,short)
        node["extensions"]["EXT_mesh_gpu_instancing"]["attributes"] = {
            k:base.accessor(model,v.astype("<f4")) for k,v in trs.items()}
        nodes.append(node)
        layers.append(trs)
    model.doc["nodes"] = nodes
    model.doc["scenes"] = [dict(nodes=list(range(len(nodes))),name="Four oaks in a meadow")]
    model.doc["scene"] = 0
    grass.write(model,ASSETS/base.MODEL.name)
    visibility(layers)
    bounds = np.array([p for n in nodes for p in node_bounds(model,n)])
    info = dict(focus=((bounds.min(axis=0)+bounds.max(axis=0))/2).tolist(), trees=trees,
                tufts=[len(t["SCALE"]) for t in layers])
    (OUT/"scene.json").write_text(json.dumps(info,indent=2))
    print(info,flush=True)


def capture(pose, mode, lighting, preview, *, viewer=VIEWER, output_dir=None, focus=None, environment=None, extra_args=()):
    (x,z),(tx,tz),pitch = CAMERAS[pose]
    y = float(height(x,z)) + EYE_HEIGHT
    horizontal = math.hypot(x-tx,z-tz)
    yaw = math.degrees(math.atan2(x-tx,z-tz))
    target = (tx,y-horizontal*math.tan(math.radians(pitch)),tz)
    base.VIEWS[pose] = (target,horizontal/math.cos(math.radians(pitch)),yaw,pitch)
    if focus is None:
        focus = json.loads((OUT/"scene.json").read_text())["focus"]
    options = ["--sun-azimuth","106","--sun-elevation","37","--exposure-ev","0.65"]
    if preview:
        options.extend(["--width","1280","--height","720"])
    elif mode=="capture" and lighting=="combined":
        options.extend(["--timing-samples","12"])
    base.run(pose,focus,mode,lighting,False,viewer=viewer,output_dir=output_dir or OUT/("draft" if preview else "renders"),
             extra_args=[*options,*extra_args],environment=environment)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("mode",choices=("prepare","capture","view"))
    parser.add_argument("--pose",choices=CAMERAS,default="canopy")
    parser.add_argument("--lighting",choices=("combined","shadow-visibility"),default="combined")
    parser.add_argument("--preview",action="store_true")
    args = parser.parse_args()
    if args.mode=="prepare":
        prepare()
    else:
        capture(args.pose,args.mode,args.lighting,args.preview)
