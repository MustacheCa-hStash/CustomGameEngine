"""Check exposed geometry with the same front-face raster rule as the UE wood."""
import csv
from pathlib import Path
import numpy as np
from ReplayOakAssembly import OUT,UE,capture,stage,mesh,quality,read_mask,write
from ScoreOakAssembly import rasterize

def run():
    src=stage("wood-source",OUT/"wood.mesh")
    candidates=list(csv.DictReader((OUT/"flat-validation.csv").open()))
    gate=[]
    result=[]
    for d in (41,100):
        cuts=sorted({r["cut"] for r in candidates if int(r["d"])==d and r["passed"]=="True"})
        if (OUT/"seed-score.csv").exists():
            for r in csv.DictReader((OUT/"seed-score.csv").open()):
                if int(r["d"])==d and float(r["iou"])>(.81 if d==41 else .45) and float(r["edge"])<(.037 if d==41 else .175) and int(r["triangles"])<(113000 if d==41 else 35000):
                    cuts.append(r["cut"])
        refs={}
        for yaw in (132,90,180):
            suffix="" if yaw==132 else f"-yaw{yaw}"
            ref=read_mask(capture(src,"input-firstface",d,yaw,one_sided=True)[0])
            ue_auto=UE/f"wood-cut-{d}{suffix}.png";ue_ref=read_mask(UE/f"wood-cut-{d}{suffix}-fine.png",True)
            target=quality(ue_auto,ue_ref,True)
            live=stage("live-readback",OUT/f"nanite{d}{suffix}.mesh")
            p,r=capture(live,"live-firstface",d,yaw,one_sided=True)
            gate.append(dict(d=d,yaw=yaw,triangles=r["wood"],cross_cut=quality(p,read_mask(ue_auto,True))[0],cross_ref=np.count_nonzero(ref&ue_ref)/np.count_nonzero(ref|ue_ref),
                             d3d_live_iou=quality(p,ref)[0],d3d_live_edge=quality(p,ref)[1],ue_iou=target[0],ue_edge=target[1]))
            refs[yaw]=(ref,target)
        write("raster-control.csv",gate)
        for cut in sorted(set(cuts)):
            viewer=stage("one-face-wood",OUT/cut)
            for yaw,(ref,(ti,te)) in refs.items():
                p,r=capture(viewer,cut.replace("/","_")+"-front",d,yaw,one_sided=True)
                qi,qe=quality(p,ref)
                result.append(dict(d=d,yaw=yaw,cut=cut,triangles=r["wood"],iou=qi,edge=qe,
                                   ue_iou=ti,ue_edge=te,passed=qi>=ti and qe<=te))
        write("one-face.csv",result)

if __name__=="__main__":run()
