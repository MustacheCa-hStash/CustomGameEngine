"""Rebuild the accepted local template recipe using the established shared-input oak fixture."""
import argparse
import hashlib
import json
from pathlib import Path
import numpy as np
from ReusableOakSprays import ROOT,REPO,load_part,save
from ScoreReusableOak import slots
from PackageReusableOak import package

RECIPE=Path(__file__).with_name("ReusableOakRecipe.json")

def prepare(spec):
    ROOT.mkdir(parents=True,exist_ok=True)
    source=REPO/"assets/source/white-oak-forked-spray"
    regions={}
    for b in spec["boughs"]:
        membership=source/f"WhiteOak-Bough{b:02d}.txt"
        tokens=membership.read_text().split()
        if hashlib.sha256(" ".join(tokens).encode()).hexdigest()!=spec["members_sha256"][str(b)]:
            raise ValueError(f"Membership differs from the measured bough {b}")
        regions[str(b)]=list(map(int,tokens[1:]))
    (ROOT/"regions.json").write_text(json.dumps(regions))
    (ROOT/"spray-bounds.json").write_text(json.dumps(spec["bounds"]))
    (v,i),(w,t)=[load_part(slot) for slot in (0,1)]
    save(ROOT/"spray.mesh",np.vstack([v,w]),np.vstack([i,t+len(v)]))
    (ROOT/"spray.mat").write_bytes(bytes(len(i))+bytes([1])*len(t))
    jobs={}
    for sample in spec["templates"].values():
        for path in (sample["leaf"],sample["wood"]):
            if path == "spray.mesh":
                continue
            family=Path(path).parts[0]
            arm,ratio=family.split("-")
            jobs[family]=dict(input="reusable/spray.mesh",arm=arm,ratio=float(ratio),output="reusable/"+family)
    (ROOT/"pilot-jobs.json").write_text(json.dumps(list(jobs.values())))

def finish(spec):
    for name,sample in spec["templates"].items():
        dest=ROOT/name
        if sample["leaf"]!=name or sample["wood"]!=name:
            lv,li,ls=slots(ROOT/sample["leaf"])
            wv,wi,ws=slots(ROOT/sample["wood"])
            leaves=li[ls==0];wood=wi[ws==1]
            dest.parent.mkdir(exist_ok=True)
            save(dest,np.vstack([lv,wv]),np.vstack([leaves,wood+len(lv)]))
            dest.with_suffix(".mat").write_bytes(bytes(len(leaves))+bytes([1])*len(wood))
        for path,key in [(dest,"geometry_sha256"),(dest.with_suffix(".mat"),"materials_sha256")]:
            if hashlib.sha256(path.read_bytes()).hexdigest()!=sample[key]:raise ValueError(f"Rebake differs from measured {path}")
    (ROOT/"policy.json").write_text(json.dumps(spec["tiers"],indent=2))
    print("Full crown:",package("pilot",spec["tiers"])/"CustomGameEngine.exe")
    for bough in spec["boughs"]:
        print(f"Bough {bough}:",package(f"pilot-{bough}",spec["tiers"],only=bough)/"CustomGameEngine.exe")

if __name__=="__main__":
    parser=argparse.ArgumentParser();parser.add_argument("task",choices=["prepare","package"])
    {"prepare":prepare,"package":finish}[parser.parse_args().task](json.loads(RECIPE.read_text()))
