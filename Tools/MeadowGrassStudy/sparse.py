"""Distance-dependent blade density, with independent directional shadow controls."""
import argparse
import json
import shutil

import clearing as previous

OUT=previous.OUT.parent/"meadow-sparse"
STAGE=OUT/"scene"
REVIEW=OUT/"review"
VIEWER=STAGE/"SparseMeadow.exe"
GROVE=previous.lod.bank.far.grove
ARMS={"dense":(), "sparse":("MEADOW_SPARSE_BLADES",),
      "sparse-contact":("MEADOW_SPARSE_BLADES","MEADOW_CONTACT_GRASS_ONLY"),
      "patch-near":("MEADOW_SPARSE_BLADES","MEADOW_PATCH_DENSITY","MEADOW_NEAR_GRASS_ONLY"),
      "patch-contact":("MEADOW_SPARSE_BLADES","MEADOW_PATCH_DENSITY","MEADOW_CONTACT_GRASS_ONLY"),
      "sward-contact":("MEADOW_SPARSE_BLADES","MEADOW_SPREAD_TUFTS","MEADOW_CONTACT_GRASS_ONLY"),
      "sward-near":("MEADOW_SPARSE_BLADES","MEADOW_SPREAD_TUFTS","MEADOW_NEAR_GRASS_ONLY"),
      "grass-shadows":("MEADOW_SPARSE_BLADES","MEADOW_TEST_NO_OAK_SHADOW")}


def stage():
    (STAGE/"assets").mkdir(parents=True,exist_ok=True)
    for src in (previous.STAGE/"assets").iterdir():
        previous.lod.bank.far.grass.copy_file(src,STAGE/"assets"/src.name)
    previous.lod.bank.far.grass.copy_file(previous.STAGE/"scene.json",STAGE/"scene.json")


def prepare():
    if not (previous.STAGE/"assets/MeadowCover.png").exists():
        previous.prepare()
    stage()
    previous.lod.bank.far.grass.lighting.prepare_viewer(upstream=previous.OUT/"source",out=OUT,
        patch=previous.lod.bank.far.grass.HERE/"sparse.patch",destination=VIEWER)


def environment(arm):
    env=previous.environment("stems")
    for flags in ARMS.values():
        for flag in flags: env.pop(flag,None)
    for flag in ARMS[arm]: env[flag]="1"
    return env


def capture(arm,pose,size,run,mode="capture",lighting="combined",viewer=VIEWER,extra_env=None):
    w,h=previous.lod.bank.SIZES[size]
    opts=["--width",str(w),"--height",str(h)]
    if mode=="capture" and lighting=="combined": opts+=["--timing-samples","36"]
    env=environment(arm)
    if extra_env: env.update(extra_env)
    GROVE.capture(pose,mode,lighting,False,viewer=viewer,extra_args=opts,
        focus=json.loads((STAGE/"scene.json").read_text())["focus"],
        output_dir=REVIEW/run/size/arm,environment=env)


def measure():
    cases=[(p,s,a) for s in ("4k","1440p") for p in ("downhill","clearing","canopy")
           for a in ("dense","sparse","sparse-contact")]
    for run,order in (("forward",cases),("reverse",reversed(cases))):
        for pose,size,arm in order:
            print(run,pose,size,arm,flush=True)
            capture(arm,pose,size,run)
    for pose in GROVE.CAMERAS:
        capture("grass-shadows",pose,"4k","diagnostic")
        for arm in ("sparse","sparse-contact"):
            capture(arm,pose,"4k","diagnostic",lighting="shadow-visibility")


def walk(arms=("dense","sparse","sparse-contact")):
    for arm in arms:
        print("walk",arm,flush=True)
        previous.lod.walk(arm,viewer=VIEWER,review=REVIEW,stage_dir=STAGE,env=environment(arm))


def trial():
    cases=[(pose,size,arm) for size in ("4k","1440p") for pose in GROVE.CAMERAS
           for arm in ("dense","sparse-contact","patch-contact","patch-near")
           if size=="4k" or pose=="downhill"]
    for run,order in (("trial1",cases),("trial2",reversed(cases))):
        for pose,size,arm in order:
            print(run,pose,size,arm,flush=True)
            capture(arm,pose,size,run)
    walk(("patch-contact","patch-near"))


def soil_check():
    # Reveal the ground to isolate grass-cast sun shadows from baked soil sky visibility.
    # The study loader treats a missing visibility texture as neutral soil sky visibility.
    no_map=OUT/"soil-without-bake"
    assets=no_map/"assets"
    assets.mkdir(parents=True,exist_ok=True)
    for src in (STAGE/"assets").iterdir():
        if src.name!="MeadowGroundVisibility.png":
            previous.lod.bank.far.grass.copy_file(src,assets/src.name)
    assert not (assets/"MeadowGroundVisibility.png").exists()
    viewer=no_map/VIEWER.name
    shutil.copy2(VIEWER,viewer)
    for arm,exe,run in (("sparse",VIEWER,"soil"),("sparse-contact",VIEWER,"soil"),
                        ("sparse-contact",viewer,"soil-no-bake")):
        capture(arm,"canopy","4k",run,viewer=exe,extra_env={"MEADOW_HIDE_ALL_GRASS_CAMERA":"1"})


def sward_trial():
    cases=[(p,s,a) for s in ("4k","1440p") for p in ("downhill","clearing","canopy")
           for a in ("dense","sward-contact","sward-near") if s=="4k" or p=="downhill"]
    for run,order in (("sward1",cases),("sward2",reversed(cases))):
        for pose,size,arm in order:
            print(run,pose,size,arm,flush=True)
            capture(arm,pose,size,run)
    walk(("sward-contact","sward-near"))


if __name__=="__main__":
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument("mode",choices=("prepare","capture","view","measure","walk","trial","soil","sward"))
    parser.add_argument("--arm",choices=ARMS,default="sward-contact")
    parser.add_argument("--pose",choices=GROVE.CAMERAS,default="downhill")
    args=parser.parse_args()
    if args.mode=="prepare": prepare()
    elif args.mode=="measure": measure()
    elif args.mode=="walk": walk()
    elif args.mode=="trial": trial()
    elif args.mode=="soil": soil_check()
    elif args.mode=="sward": sward_trial()
    else: capture(args.arm,args.pose,"4k","single",args.mode)
