"""Validate offline bough cuts in D3D on the benchmark and two held-out yaws."""
import argparse
import csv
from pathlib import Path
from statistics import median
from ReplayOakAssembly import OUT,UE,PREVIOUS,BUILD,capture,mesh,quality,read_mask,reference,stage,write

def ue_target(d,yaw):
    suffix="" if yaw==132 else "-yaw"+str(yaw)
    base=UE/f"wood-cut-{d}{suffix}-fine.png"
    candidate=UE/f"wood-cut-{d}{suffix}.png"
    return quality(candidate,read_mask(base,True),True)

def validate(scores,name):
    rows=list(csv.DictReader((OUT/scores).open()))
    out=[]
    for d in (41,100):
        exact=ue_target(d,132)
        trial=[r for r in rows if int(r["d"])==d and int(r["yaw"])==132 and
               float(r["iou"])>=exact[0]-.025 and float(r["edge"])<=exact[1]+.012 and
               int(r["triangles"])<=(170000 if d==41 else 68000)]
        for r in trial:
            f=Path(r["cut"])
            if f.parent==Path(".") and f.name.startswith("nanite"):continue
            dest=stage("trial-wood",OUT/f)
            for yaw in (132,90,180):
                target=ue_target(d,yaw)
                path,v=capture(dest,f.as_posix().replace("/","_")+"-wood",d,yaw)
                iou,edge=quality(path,reference(d,yaw))
                assert v["wood"]==int(r["triangles"])
                out.append(dict(d=d,yaw=yaw,cut=r["cut"],triangles=v["wood"],iou=iou,edge=edge,
                                ue_iou=target[0],ue_edge=target[1],passed=iou>=target[0] and edge<=target[1]))
        if out:write(name,out)

if __name__=="__main__":
    p=argparse.ArgumentParser(description=__doc__); p.add_argument("scores");p.add_argument("output")
    args=p.parse_args();validate(args.scores,args.output)
