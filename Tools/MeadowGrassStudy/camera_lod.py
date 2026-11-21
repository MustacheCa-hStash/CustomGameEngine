"""Four-oak camera LOD for tall meadow grass, with matched stills and a standing-height walk."""
import argparse
import json
import math
import subprocess
import sys
import tempfile
from pathlib import Path

import numpy as np

import camera_meadow as bank


OUT = bank.OUT.parent/"meadow-camera-lod"
STAGE = OUT/"scene"
VIEWER = STAGE/"MeadowLod.exe"
REVIEW = OUT/"review"
ARMS = ("full", "lod")
FRAMES = 97
FPS = 24


def stage():
    (STAGE/"assets").mkdir(parents=True, exist_ok=True)
    for src in (bank.STAGE/"assets").iterdir():
        bank.far.grass.copy_file(src, STAGE/"assets"/src.name)
    bank.far.grass.copy_file(bank.STAGE/"scene.json", STAGE/"scene.json")


def prepare():
    if not (bank.STAGE/"assets/MeadowCover.png").exists() or not (bank.OUT/"source").exists():
        bank.prepare()
    stage()
    bank.far.grass.lighting.prepare_viewer(upstream=bank.OUT/"source", out=OUT,
                                          patch=bank.far.grass.HERE/"camera_lod.patch", destination=VIEWER)


def environment(arm):
    env = bank.far.environment("both")
    for flag in ("MEADOW_BASE_COLOUR", "MEADOW_FULL_CAMERA_MESH", *filter(None,bank.LAYERS.values())):
        env.pop(flag,None)
    if arm=="full":
        env["MEADOW_FULL_CAMERA_MESH"]="1"
    return env


def capture(arm, pose, size="4k", mode="capture", run="forward"):
    width,height=bank.SIZES[size]
    bank.far.grove.capture(pose,mode,"combined",False,viewer=VIEWER,
                          extra_args=["--width",str(width),"--height",str(height)],
                          focus=json.loads((STAGE/"scene.json").read_text())["focus"],
                          output_dir=REVIEW/run/size/arm,environment=environment(arm))


def walk(arm, *, viewer=VIEWER, review=REVIEW, stage_dir=STAGE, env=None):
    # Walk from the canopy toward the clearing, turning slightly toward the sunny bank.
    # The target travels with the eye, which follows the terrain at 1.65 m.
    folder=review/"walk"/arm
    folder.parent.mkdir(parents=True,exist_ok=True)
    if (folder/"frames.csv").exists() and len(list(folder.glob("frame-*.png")))==FRAMES:
        return
    targets=[]
    pitch=math.radians(4)
    distance=10
    for t in np.linspace(0,1,FRAMES):
        x,z=-23.2-5.7*t,-15+3.7*t
        y=bank.far.grove.height(x,z)+bank.far.grove.EYE_HEIGHT
        yaw=math.radians(114+16*t)
        targets.append([x-distance*math.cos(pitch)*math.sin(yaw),y-distance*math.sin(pitch),
                        z-distance*math.cos(pitch)*math.cos(yaw)])
    with tempfile.TemporaryDirectory(dir=folder.parent) as tmp:
        tmp=Path(tmp)
        track=tmp/"track.txt"
        np.savetxt(track,targets,fmt="%.8f")
        args=[viewer,"--capture",tmp/"frames","--scene","forked-oak","--camera","overview",
              "--distance",str(distance),"--yaw-degrees","114","--orbit-degrees","16","--pitch-degrees","4",
              "--focus-track",track,"--width","2560","--height","1440","--msaa","4","--frames",str(FRAMES),
              "--fps",str(FPS),"--sun-azimuth","106","--sun-elevation","37","--exposure-ev","0.65",
              "--wind-strength","0","--wind-time","0","--probe-gi","bounce","--lighting","combined"]
        subprocess.run(args,cwd=stage_dir,env=environment(arm) if env is None else env,check=True,timeout=480)
        (tmp/"frames").replace(folder)


def measure():
    cases=[(pose,size,arm) for pose in bank.far.CAMERAS for size in reversed(bank.SIZES) for arm in ARMS]
    for run,order in (("forward",cases),("reverse",reversed(cases))):
        for pose,size,arm in order:
            print(run,pose,size,arm,flush=True)
            capture(arm,pose,size,run=run)
    for arm in ARMS:
        print("walk",arm,flush=True)
        walk(arm)


if __name__=="__main__":
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument("mode",choices=("prepare","capture","view","measure","walk"))
    parser.add_argument("--pose",choices=bank.far.CAMERAS,default="downhill")
    parser.add_argument("--arm",choices=ARMS,default="lod")
    args=parser.parse_args()
    if args.mode=="prepare": prepare()
    elif args.mode=="measure": measure()
    elif args.mode=="walk": walk(args.arm)
    else: capture(args.arm,args.pose,mode=args.mode)
