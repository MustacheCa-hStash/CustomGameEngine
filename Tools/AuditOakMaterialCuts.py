"""Test separate material budgets around the measured live Nanite allocation."""
import numpy as np
from CompareOakJoint import finalist, front
from ScoreOakAssembly import rasterize
from ReplayOakAssembly import OUT, UE, measure, quality, read_mask, write

def layers(parts,d,yaw):
    leaf,(v,i)=parts
    return rasterize(leaf,d,yaw),rasterize((v,i[front(v,i,d,yaw)]),d,yaw)

def audit():
    rows=[]
    for d in (41,100):
        leaves = [OUT/f"leaf-{arm}-mixedtest/level{3 if d==41 else 4:02d}.mesh" for arm in ("epic","custom")]
        wood = (["seed-custom01-epic/level01.mesh","instanced-custom-02.mesh","epic-0.5/level02.mesh"]
                if d==41 else ["epic-0.5/level04-paired.mesh","seed-epic03-custom/level01.mesh",
                               "epic-0.68/level07.mesh","epic-0.68/level07-paired.mesh","custom-0.68/level06.mesh"])
        for yaw in (132,90,180):
            refs=layers(finalist(OUT/"leaf.mesh",OUT/"wood.mesh"),d,yaw)
            r=np.minimum(*refs)
            sy,sx=np.where(np.isfinite(r))
            crop=np.s_[sy.min()-24:sy.max()+25,sx.min()-24:sx.max()+25]
            ref=r[crop].copy()
            suffix="" if yaw==132 else f"-yaw{yaw}"
            ti,te=quality(UE/f"joint-cut-{d}{suffix}.png",read_mask(UE/f"joint-cut-{d}{suffix}-fine.png",True),True)
            def score(name,leaf,wood,parts):
                leafz,woodz=layers(parts,d,yaw)
                z=np.minimum(leafz[crop],woodz[crop])
                occ=np.isfinite(z)
                m=next(measure(np.isfinite(ref),[(0,0,occ)]))
                shared=np.isfinite(ref)&occ
                rows.append(dict(d=d,yaw=yaw,cut=name,leaf=leaf,wood=wood,leaves=len(parts[0][1]),twigs=len(parts[1][1]),
                                 iou=m[2],edge=m[-1],passed=m[2]>=ti and m[-1]<=te,
                                 leaf_only_iou=next(measure(np.isfinite(refs[0][crop]),[(0,0,np.isfinite(leafz[crop]))]))[2],
                                 wood_only_iou=next(measure(np.isfinite(refs[1][crop]),[(0,0,np.isfinite(woodz[crop]))]))[2],
                                 p95_mm=np.percentile(abs(z[shared]-ref[shared])*1000,95)))
            for a in leaves:
                for b in wood:
                    score("separate",a.relative_to(OUT).as_posix(),b,finalist(a,OUT/b))
            if yaw==132:
                score("live-Nanite","","",finalist(OUT/f"joint-nanite{d}.mesh"))
    write("material-audit.csv",rows)

if __name__=="__main__":audit()
