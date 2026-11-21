"""Four-oak far-caster and ground-material A/B. All arms share one camera mesh."""
import argparse
import json
import os
import subprocess
import sys

import numpy as np
from PIL import Image

import grass_tiles as tiles
from grass_tiles import base, grass, grove


OUT = base.ROOT/"build/meadow-far-shadow"
STAGE = OUT/"scene"
ARMS = ("reference","shadow","ground","both")
CAMERAS = grove.CAMERAS


def growth_map(stage=STAGE):
    """RG stores dry litter and living cover. Rasterize the placed tall/short tufts in world space."""
    model = base.oak.OakGlb(stage/"assets"/base.MODEL.name)
    size,span = 640,80
    step=span/size
    area=np.zeros((size,size),float)
    for node in model.doc["nodes"]:
        short=node["name"].startswith("Short grass")
        if not short and not node["name"].startswith("Full grass strip"):
            continue
        attrs=node["extensions"]["EXT_mesh_gpu_instancing"]["attributes"]
        pos=model.array(attrs["TRANSLATION"])
        scale=model.array(attrs["SCALE"])
        x,z=np.floor((pos[:,[0,2]]+48)/step).astype(int).T
        np.add.at(area,(z,x),scale[:,0]*scale[:,2]*(.021 if short else .069)/step**2)
    freq=2*np.pi*np.fft.fftfreq(size,d=step)
    spectrum=np.fft.fft2(area)
    square=freq[:,None]**2+freq[None,:]**2
    def smooth(metres):
        return np.maximum(0,np.fft.ifft2(spectrum*np.exp(-.5*metres**2*square)).real)
    fine,broad=smooth(.44),smooth(1.6)
    x,z=np.meshgrid((np.arange(size)+.5)*step-48,(np.arange(size)+.5)*step-48)
    trees=json.loads((stage/"scene.json").read_text())["trees"]
    rootdist=np.minimum.reduce([np.hypot(x-tx,z-tz) for tx,tz in trees])
    roots=np.clip((rootdist-.85)/2.25,0,1)
    roots=roots*roots*(3-2*roots)
    live=np.clip((.65*fine+.35*broad-.15)/.91,0,1)*roots
    live*=.79+.10*np.sin(.56*x+.42*z)+.05*np.cos(1.3*z-.3*x)
    dry=np.clip((.6*broad+.4*fine)/.6,0,1)*(.44+.15*np.cos(.52*z-.4*x))
    dry*=np.clip((rootdist-.65)/1.1,0,1)
    mask=np.stack((dry,live,np.zeros_like(live)),axis=-1)
    Image.fromarray(np.uint8(255*np.clip(mask,0,1)+.5)).save(stage/"assets/MeadowCover.png")


def prepare():
    if not (tiles.OUT/"visual/scene.json").exists():
        subprocess.run([sys.executable,"-B",tiles.__file__,"prepare"],check=True)
    (STAGE/"assets").mkdir(parents=True,exist_ok=True)
    grass.lighting.prepare_viewer(upstream=tiles.OUT/"source",out=OUT,patch=grass.HERE/"far_shadow.patch",
                                  destination=STAGE/"FarGrass.exe")
    for src in (tiles.OUT/"visual/assets").iterdir():
        grass.copy_file(src,STAGE/"assets"/src.name)
    grass.copy_file(tiles.OUT/"visual/scene.json",STAGE/"scene.json")
    # Power-of-two dimensions keep the complete mip chain for the repeating colour tile.
    with Image.open(grass.SOURCE/"meadow-sward.png") as tile:
        tile.convert("RGB").resize((1024,1024),Image.Resampling.LANCZOS).save(STAGE/"assets/MeadowSward.png")
    growth_map()


def environment(arm,check=None):
    env=os.environ.copy()
    for flag in (*tiles.FLAGS,"MEADOW_SOIL_ONLY","MEADOW_FULL_FAR_SHADOW","MEADOW_UNIFORM_BOUNCE",
                 "MEADOW_TEST_FAR_CAMERA_TUFTS"):
        env.pop(flag,None)
    if arm in ("reference","ground"):
        env["MEADOW_FULL_FAR_SHADOW"]="1"
    if arm in ("reference","shadow"):
        env["MEADOW_SOIL_ONLY"]="1"
    if check:
        env[check]="1"
    return env


def capture(arm,pose,mode="capture",lighting="combined",out=None,check=None,preview=False):
    grove.capture(pose,mode,lighting,preview,viewer=STAGE/"FarGrass.exe",
                  focus=json.loads((STAGE/"scene.json").read_text())["focus"],
                  output_dir=out or OUT/"review"/arm,environment=environment(arm,check))


if __name__=="__main__":
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument("mode",choices=("prepare","capture","view"))
    parser.add_argument("--arm",choices=ARMS,default="both")
    parser.add_argument("--pose",choices=CAMERAS,default="canopy")
    args=parser.parse_args()
    if args.mode=="prepare": prepare()
    else: capture(args.arm,args.pose,args.mode)
