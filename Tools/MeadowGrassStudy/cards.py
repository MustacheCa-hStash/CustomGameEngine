"""Dense four-oak meadow with independently testable camera culling and tuft cards."""
import argparse
import json
import numpy as np
from PIL import Image
import sparse
from tuft_cards import append_card

OUT=sparse.OUT.parent/"meadow-cards"
STAGE=OUT/"scene"
VIEWER=STAGE/"MeadowCards.exe"
REVIEW=OUT/"review"
GROVE=sparse.GROVE
ARMS=("unculled","geometry","cards")
far=sparse.previous.lod.bank.far


def stage():
    assets=STAGE/"assets"
    assets.mkdir(parents=True,exist_ok=True)
    for path in (sparse.STAGE/"assets").iterdir():
        if path.name not in (far.base.MODEL.name,"MeadowCover.png","MeadowGroundVisibility.png"):
            far.grass.copy_file(path,assets/path.name)
    info=json.loads((sparse.STAGE/"scene.json").read_text())
    model=far.base.oak.OakGlb(sparse.STAGE/"assets"/far.base.MODEL.name)
    nodes={n["name"]:n for n in model.doc["nodes"]}
    layers=[]
    for short in (False,True):
        extra=GROVE.scatter(info["trees"],short,88716+int(short))
        retain=np.random.default_rng(1190+int(short)).random(len(extra["SCALE"])) < (.35 if short else .55)
        extra={k:v[retain] for k,v in extra.items()}
        x,_,z=extra["TRANSLATION"].T
        low=far.tiles.lower_growth(x,z)
        extra["SCALE"][:,1]*=1-(.32 if short else .65)*low
        if short:
            extra["SCALE"][:,[0,2]]*=(1+.34*low)[:,None]
            groups=[("Short grass",np.ones(len(x),bool))]
        else:
            ripe=far.tiles.flowering_mask(x,z,info["trees"])
            groups=[("Full grass strip",~ripe),("Full grass strip | ripe stands",ripe)]
            info["floweringTufts"]+=int(ripe.sum())
            info["tallTuftsLoweredAtLeast30Pct"]+=int((low*.65>=.30).sum())
        stands=[]
        for name,select in groups:
            node=nodes[name]
            old=node["extensions"]["EXT_mesh_gpu_instancing"]["attributes"]
            trs={k:np.concatenate((model.array(a),extra[k][select])).astype("<f4") for k,a in old.items()}
            far.tiles.set_instances(model,node,trs)
            stands.append(trs)
            print("Baking",name,len(trs["SCALE"]),flush=True)
            append_card(model,node)
        merged={k:np.concatenate([s[k] for s in stands]) for k in stands[0]}
        layers.append(merged)
        info["tufts"][int(short)]=len(merged["SCALE"])
    far.grass.write(model,assets/far.base.MODEL.name)
    (STAGE/"scene.json").write_text(json.dumps(info,indent=2))
    GROVE.visibility(layers,assets)
    far.growth_map(STAGE)
    with Image.open(assets/"MeadowCover.png") as current, Image.open(sparse.STAGE/"assets/MeadowCover.png") as old:
        merged=Image.merge("RGB",(*current.split()[:2],old.getchannel("B")))
    merged.save(assets/"MeadowCover.png")
    print(info,flush=True)


def build_viewer():
    far.grass.lighting.prepare_viewer(upstream=sparse.OUT/"source",out=OUT,
        patch=far.grass.HERE/"cards.patch",destination=VIEWER)


def prepare():
    if not (sparse.STAGE/"assets"/far.base.MODEL.name).exists() or not (sparse.OUT/"source").is_dir():
        sparse.prepare()
    stage()
    build_viewer()


def env(arm):
    result=sparse.environment("dense")
    result["MEADOW_CONTACT_GRASS_ONLY"]="1"
    for flag in ("MEADOW_UNCULLED_OAK_CAMERA","MEADOW_ALPHA_CARDS"):
        result.pop(flag,None)
    if arm=="unculled": result["MEADOW_UNCULLED_OAK_CAMERA"]="1"
    if arm=="cards": result["MEADOW_ALPHA_CARDS"]="1"
    return result


def capture(arm,pose,size="4k",run="single",mode="capture",lighting="combined",flags=None):
    w,h=sparse.previous.lod.bank.SIZES[size]
    options=["--width",str(w),"--height",str(h)]
    if mode=="capture" and lighting=="combined": options.extend(["--timing-samples","36"])
    environment=env(arm)
    if flags: environment.update(flags)
    GROVE.capture(pose,mode,lighting,False,viewer=VIEWER,extra_args=options,
        focus=json.loads((STAGE/"scene.json").read_text())["focus"],
        output_dir=REVIEW/run/size/arm,environment=environment)


def oak_check():
    for run,arms in (("oak1",("unculled","geometry")),("oak2",("geometry","unculled"))):
        for pose in GROVE.CAMERAS:
            for arm in arms:
                print(run,pose,arm,flush=True)
                capture(arm,pose,run=run)
    capture("geometry","clearing",run="oak-shadow-off",flags={"MEADOW_TEST_NO_OAK_SHADOW":"1"})


if __name__=="__main__":
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument("mode",choices=("capture","view","oak-check","shadow-check","stage","prepare","walk","measure"))
    parser.add_argument("--arm",choices=ARMS,default="cards")
    parser.add_argument("--pose",choices=GROVE.CAMERAS,default="clearing")
    args=parser.parse_args()
    if args.mode=="prepare": prepare()
    elif args.mode=="stage": stage()
    elif args.mode=="oak-check": oak_check()
    elif args.mode=="shadow-check":
        capture("cards","clearing",run="final-no-oak-shadow",flags={"MEADOW_TEST_NO_OAK_SHADOW":"1"})
        capture("cards","clearing",run="final-no-grass",flags={"MEADOW_HIDE_ALL_GRASS_CAMERA":"1"})
        capture("cards","downhill",run="final-oak-control",flags={"MEADOW_UNCULLED_OAK_CAMERA":"1"})
    elif args.mode=="walk":
        sparse.previous.lod.walk(args.arm,viewer=VIEWER,review=REVIEW,stage_dir=STAGE,env=env(args.arm))
    elif args.mode=="measure":
        cases=[(p,s,a) for s in ("4k","1440p") for p in GROVE.CAMERAS for a in ("geometry","cards")]
        for run,order in (("forward",cases),("reverse",reversed(cases))):
            for p,s,a in order:
                print(run,p,s,a,flush=True)
                capture(a,p,s,run)
    else: capture(args.arm,args.pose,mode=args.mode)
