"""Render the four-oak meadow with independently selectable layout and culling controls."""
import argparse
import copy
import json
import os
import subprocess
import sys

import numpy as np

import four_oaks as grove
from four_oaks import base, grass


OUT = base.ROOT / "build/meadow-tiles"
VARIANTS = ("before", "frustum", "culled", "meadow")
FLAGS = ("MEADOW_DISABLE_TILES", "MEADOW_FRUSTUM_ONLY", "MEADOW_TEST_CAMERA_SHADOWS",
         "MEADOW_TEST_NO_OAK_CAMERA", "MEADOW_TEST_NO_OAK_SHADOW", "MEADOW_TEST_KEEP_ALL_TILES",
         "MEADOW_TEST_CAMERA_TUFTS")


def flowering_mesh(model, mesh_index):
    mesh = copy.deepcopy(model.doc["meshes"][mesh_index])
    mesh["name"] = "Meadow tuft with ripe seed heads"
    colours = {"Glume | olive": (.47,.37,.16), "Glume | chaff": (.68,.52,.25),
               "Glume | flushed": (.46,.30,.14), "Green sheath": (.40,.39,.17)}
    for prim in mesh["primitives"]:
        attr = prim["attributes"]
        pos = model.array(attr["POSITION"])
        normal = model.array(attr["NORMAL"])
        # Extend the upper culm and open the panicle. The same continuous deformation
        # across all material seams keeps the sheaths, leaves and spikelets attached.
        u = np.maximum(pos[:,1]-.35,0)
        spread = 1 + .78*u*u
        deriv = 1 + 2.35*u
        normal[:,[0,2]] /= spread[:,None]
        normal[:,1] = (normal[:,1] - 1.56*u*(pos[:,0]*normal[:,0]+pos[:,2]*normal[:,2])) / deriv
        normal /= np.linalg.norm(normal,axis=1,keepdims=True)
        pos[:,[0,2]] *= spread[:,None]
        pos[:,1] += 1.175*u*u
        attr["POSITION"] = base.accessor(model,pos)
        attr["NORMAL"] = base.accessor(model,normal)
        source = model.doc["materials"][prim["material"]]
        if source["name"] in colours:
            mat = copy.deepcopy(source)
            mat["pbrMetallicRoughness"]["baseColorFactor"] = [*colours[source["name"]],1]
            mat["name"] += " | ripe"
            prim["material"] = len(model.doc["materials"])
            model.doc["materials"].append(mat)
    model.doc["meshes"].append(mesh)
    return len(model.doc["meshes"])-1


def lower_growth(x,z):
    # A broad, winding strip follows the low ground between the two eastern banks.
    axis = -5 + .25*z + 1.5*np.sin(z/7)
    main = np.exp(-((x-axis)/4.8)**2-((z+10)/20)**2)
    west = np.exp(-((x+26-.30*(z+15))/3.1)**2-((z+15)/9)**2)
    return np.maximum(main,west)


def flowering_mask(x,z,trees):
    # An irregular set of stands on the shoulders of the meadow, with gaps between.
    pockets = [(-32,-11,3.4,2.2),(-40,-4,3.2,2.6),(-34,-2,2.8,2.1),
               (-17,-17,2.2,2.7),(-12,-11,2.7,1.8),(-3,-19,3.1,2.5),
               (9,-8,4,3),(-15,7,3,2),(-23,-31,4,3),(2,-34,3,2)]
    clusters = np.maximum.reduce([np.exp(-((x-cx)/rx)**2-((z-cz)/rz)**2) for cx,cz,rx,rz in pockets])
    # Approximate the open fraction from the projected crown centres at this study sun.
    shade = np.maximum.reduce([np.exp(-((x-(tx-12.8))/7)**2-((z-(tz+3.6))/6.3)**2) for tx,tz in trees])
    chance = np.clip((.012+.94*clusters)*(1-.95*shade)*(1-lower_growth(x,z)),0,1)
    return np.random.default_rng(8219).random(len(x)) < chance


def set_instances(model,node,trs):
    node["extensions"]["EXT_mesh_gpu_instancing"]["attributes"] = {
        key:base.accessor(model,array.astype("<f4")) for key,array in trs.items()}


def prepare():
    if not (grove.OUT/"scene.json").exists():
        subprocess.run([sys.executable,"-B",grove.__file__,"prepare"],check=True)
    OUT.mkdir(parents=True,exist_ok=True)
    viewer = grass.lighting.prepare_viewer(upstream=grove.OUT/"source",out=OUT,
                                           patch=grass.HERE/"grass_tiles.patch",destination=OUT/"GrassTiles.exe")
    for folder in ("baseline","visual"):
        stage = OUT/folder
        assets = stage/"assets"
        assets.mkdir(parents=True,exist_ok=True)
        for path in grove.ASSETS.iterdir():
            if folder=="visual" and path.name in (base.MODEL.name,"MeadowGroundVisibility.png"):
                continue
            grass.copy_file(path,assets/path.name)
        grass.copy_file(viewer,stage/viewer.name)
    model = base.oak.OakGlb(grove.ASSETS/base.MODEL.name)
    scene = json.loads((grove.OUT/"scene.json").read_text())
    layers = []
    for name,short in (("Full grass strip",False),("Short grass",True)):
        node = next(n for n in model.doc["nodes"] if n["name"]==name)
        trs = {k:model.array(a) for k,a in node["extensions"]["EXT_mesh_gpu_instancing"]["attributes"].items()}
        x,_,z = trs["TRANSLATION"].T
        low = lower_growth(x,z)
        trs["SCALE"][:,1] *= 1-(.32 if short else .65)*low
        layers.append(trs)
        if short:
            trs["SCALE"][:,[0,2]] *= (1+.34*low)[:,None]
            set_instances(model,node,trs)
            continue
        ripe = flowering_mask(x,z,scene["trees"])
        mature = copy.deepcopy(node)
        mature["name"] = "Full grass strip | ripe stands"
        mature["mesh"] = flowering_mesh(model,node["mesh"])
        set_instances(model,mature,{k:v[ripe] for k,v in trs.items()})
        set_instances(model,node,{k:v[~ripe] for k,v in trs.items()})
        model.doc["scenes"][0]["nodes"].append(len(model.doc["nodes"]))
        model.doc["nodes"].append(mature)
        scene["floweringTufts"] = int(ripe.sum())
        scene["tallTuftsLoweredAtLeast30Pct"] = int(np.count_nonzero(low*.65>=.30))
    grove.visibility(layers,OUT/"visual/assets")
    grass.write(model,OUT/"visual/assets"/base.MODEL.name)
    bounds = np.array([point for n in model.doc["nodes"] for point in grove.node_bounds(model,n)])
    scene["focus"] = ((bounds.min(axis=0)+bounds.max(axis=0))/2).tolist()
    (OUT/"visual/scene.json").write_text(json.dumps(scene,indent=2))


def capture(variant,pose,mode,lighting="combined",directory=None,diagnostic=None):
    env = os.environ.copy()
    for flag in FLAGS:
        env.pop(flag,None)
    if variant=="before":
        env["MEADOW_DISABLE_TILES"]="1"
    if variant=="frustum":
        env["MEADOW_FRUSTUM_ONLY"]="1"
    if diagnostic:
        env[diagnostic]="1"
    stage = OUT/("visual" if variant=="meadow" else "baseline")
    ref = stage/"scene.json" if variant=="meadow" else grove.OUT/"scene.json"
    grove.capture(pose,mode,lighting,False,viewer=stage/"GrassTiles.exe",
                  output_dir=directory or OUT/variant,focus=json.loads(ref.read_text())["focus"],environment=env)


if __name__=="__main__":
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument("mode",choices=("prepare","capture","view"))
    parser.add_argument("--variant",choices=VARIANTS,default="meadow")
    parser.add_argument("--pose",choices=grove.CAMERAS,default="canopy")
    args=parser.parse_args()
    if args.mode=="prepare":
        prepare()
    else:
        capture(args.variant,args.pose,args.mode)
