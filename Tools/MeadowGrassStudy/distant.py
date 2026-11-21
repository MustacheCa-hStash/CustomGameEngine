"""Fit the eight distant grass silhouettes and measure them in the four-oak ribbon meadow."""
import argparse
import io
import json
from pathlib import Path
import shutil

import numpy as np
from PIL import Image

import tuft_models as tuft

OUT = tuft.OUT.parent / "meadow-distant"
EXE = OUT / "DistantMeadow.exe"
HERE = Path(__file__).resolve().parent
ARMS = ("reference","outline","depth","shared","vertex","optimized","near","bare")


def pack_outline(model, prim):
    """Store eight outward-rounded U limits in two exactly representable 24-bit integers."""
    attrs = prim["attributes"]
    uv = model.array(attrs["TEXCOORD_0"])
    assert len(uv)==18 and np.allclose(uv[::2,1],uv[1::2,1])
    mat = model.doc["materials"][prim["material"]]
    tex = mat["pbrMetallicRoughness"]["baseColorTexture"]["index"]
    img = model.doc["images"][model.doc["textures"][tex]["source"]]
    buf = model.doc["bufferViews"][img["bufferView"]]
    start = buf.get("byteOffset",0)
    with Image.open(io.BytesIO(model.payload[start:start+buf["byteLength"]])) as atlas:
        alpha = np.array(atlas.getchannel("A"))
    cell = alpha.shape[0]//2
    assert alpha.shape==(2*cell,4*cell)
    packed = np.zeros((len(uv),2),np.uint32)
    for view in range(8):
        x,y=view%4*cell,view//4*cell
        tile=alpha[y:y+cell,x:x+cell]
        for row in range(len(uv)//2):
            # Using both adjoining strips makes the interpolated trapezoid conservative.
            v=uv[2*max(0,row-1):2*min(9,row+2):2,1]
            a=max(0,int(np.floor(v.min()*cell))-3)
            b=min(cell,int(np.ceil(v.max()*cell))+3)
            cols=np.flatnonzero(tile[a:b].any(axis=0))
            edges=(np.clip([(cols.min()-5)/cell,(cols.max()+6)/cell],0,1)
                   if len(cols) else [.5,.5])
            limits=np.array([np.floor(edges[0]*63),np.ceil(edges[1]*63)],np.uint32)
            packed[row*2:row*2+2,view//4] |= limits << (6*(view%4))
    attrs["TEXCOORD_1"]=tuft.BASE.accessor(model,packed.astype("<f4"))


def stage():
    assets=OUT/"assets"
    assets.mkdir(parents=True,exist_ok=True)
    if not (tuft.ASSETS/tuft.BASE.MODEL.name).exists() or not (tuft.OUT/"source").exists():
        import subprocess
        import sys
        subprocess.run([sys.executable,HERE/"tuft_models.py","prepare"],check=True)
    for path in tuft.ASSETS.iterdir():
        if path.name!=tuft.BASE.MODEL.name:
            shutil.copy2(path,assets/path.name)
    model=tuft.BASE.oak.OakGlb(tuft.ASSETS/tuft.BASE.MODEL.name)
    meshes={node["mesh"] for node in model.doc["nodes"] if node["name"].startswith("Meadow tuft card")}
    for index in meshes:
        for prim in model.doc["meshes"][index]["primitives"]:
            pack_outline(model,prim)
    tuft.cards.far.grass.write(model,assets/tuft.BASE.MODEL.name)


def environment(arm):
    env=tuft.cards.env("cards")
    for flag in ("MEADOW_FAR_OUTLINE","MEADOW_FAR_DEPTH","MEADOW_FAR_VERTEX_PROBES",
                 "MEADOW_FAR_SHARED_DEPTH","MEADOW_HIDE_FAR_CARDS","MEADOW_TUFT_PILOT","MEADOW_TUFT_SLICES",
                 "MEADOW_CARDS_TO_ROOT","MEADOW_HIDE_ALL_GRASS_CAMERA"):
        env.pop(flag,None)
    env["MEADOW_TUFT_RIBBONS"]="1"
    if arm in ("outline","shared","vertex","optimized"): env["MEADOW_FAR_OUTLINE"]="1"
    if arm in ("depth","shared","optimized"): env["MEADOW_FAR_DEPTH"]="1"
    if arm in ("shared","optimized"): env["MEADOW_FAR_SHARED_DEPTH"]="1"
    if arm in ("vertex","optimized"): env["MEADOW_FAR_VERTEX_PROBES"]="1"
    if arm=="near": env["MEADOW_HIDE_FAR_CARDS"]="1"
    if arm=="bare": env["MEADOW_HIDE_ALL_GRASS_CAMERA"]="1"
    return env


def capture(arm,pose,run="single",mode="capture",counters=False):
    grove=tuft.cards.GROVE
    grove.CAMERAS["foreground"]=((-26.,-13.),(-30.,-10.),32.)
    options=tuft.timing_options(mode,counters)
    grove.capture(pose,mode,"combined",False,viewer=EXE,environment=environment(arm),
        focus=json.loads((tuft.cards.STAGE/"scene.json").read_text())["focus"],extra_args=options,
        output_dir=OUT/"review"/run/arm)


if __name__=="__main__":
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument("mode",choices=("prepare","stage","view","capture","measure","walk","counters"))
    ap.add_argument("--arm",choices=ARMS,default="optimized")
    ap.add_argument("--pose",choices=tuft.POSES,default="clearing")
    args=ap.parse_args()
    if args.mode in ("stage","prepare"):
        stage()
        if args.mode=="prepare":
            tuft.cards.far.grass.lighting.prepare_viewer(upstream=tuft.OUT/"source",out=OUT,
                patch=HERE/"distant.patch",destination=EXE)
    elif args.mode=="walk":
        tuft.cards.sparse.previous.lod.walk(args.arm,viewer=EXE,stage_dir=OUT,review=OUT/"review",env=environment(args.arm))
    elif args.mode=="measure":
        cases=[(a,p) for p in ("canopy","clearing","downhill") for a in ("near","reference","optimized","bare")]
        for run,order in (("forward",cases),("reverse",reversed(cases))):
            for arm,pose in order:
                print(run,arm,pose,flush=True)
                capture(arm,pose,run)
    else:
        capture(args.arm,args.pose,"counters" if args.mode=="counters" else "single",
                "view" if args.mode=="view" else "capture",args.mode=="counters")
