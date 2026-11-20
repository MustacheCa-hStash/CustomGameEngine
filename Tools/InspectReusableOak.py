"""Audit which templates are selected, per-material triangles and foreground depths."""
import csv
import json
import numpy as np
from ReusableOakSprays import ROOT,expand_parts,table
from PackageReusableOak import layout
from PackageOakAssembly import pixel_scale
from ScoreReusableOak import depth,slots,setup,measure

def run():
    policy=json.loads((ROOT/"policy.json").read_text())
    rows=[];usage=[]
    hardware={(int(r["b"]),int(r["d"]),int(r["yaw"])):r for r in csv.DictReader((ROOT/"pilot-masks.csv").open())}
    for b,groups in layout().items():
        ids=np.concatenate([g[1] for g in groups])
        for d in (41,100):
            for yaw in (132,90,180,45,225,0,270):
                crop,_=setup(b,d,yaw)
                shape=(crop[3]-crop[1],crop[2]-crop[0])
                def layers(items):
                    surfaces=np.full((2,*shape),np.inf,np.float32);counts=[0,0]
                    for path,members in items:
                        v,i,s=slots(ROOT/path)
                        for k in (0,1):
                            vv,ii=(v,i[s==k]) if members is None else expand_parts((v,i[s==k]),members)
                            counts[k]+=len(ii)
                            z=depth(vv,ii,np.full(len(ii),k,np.uint8),d,yaw,crop)
                            np.minimum(surfaces[k],z,out=surfaces[k])
                    return surfaces,counts
                buckets={}
                for first,group,n in groups:
                    level=next((p for t,p in policy if pixel_scale(n,d,yaw)<=t*.9999),"spray.mesh")
                    buckets.setdefault(level,[]).extend(group)
                source,_=layers([("spray.mesh",ids)]);ref=np.minimum(*source)
                cases=[("pilot",list(buckets.items()))]
                if yaw==132:cases.append(("live Nanite",[(f"live{b}-{d}.mesh",None)]))
                for name,items in cases:
                    z,counts=layers(items)
                    if name=="pilot":
                        expected=int(hardware[b,d,yaw]["tri"])
                        assert sum(counts)==expected,(b,d,yaw,counts,expected)
                        usage.extend(dict(b=b,d=d,yaw=yaw,template=k,placements=len(v)) for k,v in items)
                    nearest=np.minimum(*z);shared=np.isfinite(ref)&np.isfinite(nearest)
                    delta=abs(nearest[shared]-ref[shared])*1000
                    row=dict(b=b,d=d,yaw=yaw,cut=name,leaves=counts[0],wood=counts[1],p50_mm=np.median(delta),
                             p95_mm=np.percentile(delta,95),over_100mm=np.mean(delta>100))
                    for k,key in enumerate(("leaf_iou","wood_iou")):
                        row[key]=next(measure(np.isfinite(source[k]),[(0,0,np.isfinite(z[k]))]))[2]
                    rows.append(row)
    table("depth-materials.csv",rows);table("selection.csv",usage)

if __name__=="__main__":run()
