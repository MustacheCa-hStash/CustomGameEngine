"""Matched camera-only geometry and shading controls for the current clearing LOD."""
import argparse
import json

import camera_lod as lod

OUT = lod.OUT.parent/"meadow-clearing"
STAGE = OUT/"scene"
REVIEW = OUT/"review"
VIEWER = STAGE/"Clearing.exe"
ARMS = {
    "reference": (),
    "no-blades": ("MEADOW_HIDE_BLADES",),
    "no-stems": ("MEADOW_HIDE_STEMS",),
    "no-heads": ("MEADOW_HIDE_HEADS",),
    "no-normal": ("MEADOW_NO_BLADE_NORMAL",),
    "no-glint": ("MEADOW_NO_BLADE_GLINT",),
    "no-surface": ("MEADOW_NO_BLADE_NORMAL","MEADOW_NO_BLADE_GLINT"),
    "stems": ("MEADOW_LIGHT_STEMS",),
}


def stage():
    (STAGE/"assets").mkdir(parents=True,exist_ok=True)
    for src in (lod.STAGE/"assets").iterdir():
        lod.bank.far.grass.copy_file(src,STAGE/"assets"/src.name)
    lod.bank.far.grass.copy_file(lod.STAGE/"scene.json",STAGE/"scene.json")


def prepare():
    if not (lod.STAGE/"assets/MeadowCover.png").exists():
        lod.prepare()
    stage()
    lod.bank.far.grass.lighting.prepare_viewer(upstream=lod.OUT/"source",out=OUT,
        patch=lod.bank.far.grass.HERE/"clearing.patch",destination=VIEWER)


def environment(arm):
    env=lod.environment("lod")
    for flags in ARMS.values():
        for flag in flags:
            env.pop(flag,None)
    for flag in ARMS[arm]:
        env[flag]="1"
    return env


def capture(arm,size,run,mode="capture",viewer=VIEWER,samples=12,pose="clearing"):
    w,h=lod.bank.SIZES[size]
    lod.bank.far.grove.capture(pose,mode,"combined",False,viewer=viewer,
        extra_args=["--width",str(w),"--height",str(h),*(["--timing-samples",str(samples)] if mode=="capture" else [])],
        focus=json.loads((STAGE/"scene.json").read_text())["focus"],
        output_dir=REVIEW/run/size/arm,environment=environment(arm))


def measure(arms):
    for k in range(3):
        order=list(arms) if k==0 else list(reversed(arms)) if k==1 else [*arms[3:],*arms[:3]]
        for size in (("4k","1440p") if k!=1 else ("1440p","4k")):
            for arm in order:
                print(k,size,arm,flush=True)
                capture(arm,size,f"run{k+1}")


def walk(arms,viewer=VIEWER):
    for arm in arms:
        print("walk",arm,flush=True)
        lod.walk(arm,viewer=viewer,review=REVIEW,stage_dir=STAGE,env=environment(arm))


def trial(viewer=VIEWER):
    for k in range(4):
        for size in (("4k","1440p") if k%2==0 else ("1440p","4k")):
            for arm in (("reference","stems") if k%2==0 else ("stems","reference")):
                print(k,size,arm,flush=True)
                capture(arm,size,f"trial{k+1}",viewer=viewer,samples=72)
    walk(["stems"],viewer)


if __name__=="__main__":
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument("mode",choices=("prepare","capture","view","walk","measure","trial"))
    parser.add_argument("--arms",nargs="+",choices=ARMS)
    parser.add_argument("--size",choices=lod.bank.SIZES,default="4k")
    parser.add_argument("--pose",choices=lod.bank.far.CAMERAS,default="clearing")
    args=parser.parse_args()
    if args.mode=="prepare": prepare()
    elif args.mode=="measure": measure(args.arms or [a for a in ARMS if a!="stems"])
    elif args.mode=="walk": walk(args.arms or ["reference","no-normal","no-glint","no-surface","stems"])
    elif args.mode=="trial": trial()
    else:
        for arm in args.arms or ["reference"]: capture(arm,args.size,"single",args.mode,pose=args.pose)
