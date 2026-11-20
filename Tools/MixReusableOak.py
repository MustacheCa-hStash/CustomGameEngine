"""Test different leaf and wood budgets in a jointly selected reusable template."""
import csv
import json
import numpy as np
from PackageReusableOak import layout
from ScoreReusableOak import ROOT,slots,setup,depth,measure,expand_parts,save,table

def run():
    previous=list(csv.DictReader((ROOT/"template-screen.csv").open()))
    regions=layout(); candidates=[]
    for d in (41,100):
        catalogue=sorted({r["cut"] for r in previous if r["d"]==str(d) and not r["cut"].startswith("native")})
        dims={p:np.bincount(slots(ROOT/p)[2],minlength=2) for p in catalogue}
        layers={};grade={}
        for b,groups in regions.items():
            ids=np.concatenate([g[1] for g in groups]);crop,gate=setup(b,d,132)
            masks=[]
            for slot in (0,1):
                buffers={}
                for p in ["spray.mesh",*catalogue]:
                    v,i,s=slots(ROOT/p)
                    v,i=expand_parts((v,i[s==slot]),ids)
                    z=depth(v,i,np.full(len(i),slot,np.uint8),d,132,crop)
                    buffers[p]=np.isfinite(z)
                    if p!="spray.mesh":
                        m=next(measure(buffers["spray.mesh"],[(0,0,buffers[p])]))
                        grade[b,slot,p]=m[2]-2*m[-1]
                masks.append(buffers)
            ref=masks[0]["spray.mesh"]|masks[1]["spray.mesh"]
            layers[b]=(ref,gate,masks)
        pools=[]
        for slot in (0,1):
            acc=set()
            for b in regions:
                best=-100
                for p in sorted(catalogue,key=lambda p:(dims[p][slot],-grade[b,slot,p])):
                    if grade[b,slot,p]>best+.0003:
                        best=grade[b,slot,p];acc.add(p)
            pools.append(sorted(acc))
        tested=[]
        for a in pools[0]:
            for w in pools[1]:
                n=int(dims[a][0]+dims[w][1])
                if not ((245<=n<=505) if d==41 else (92<=n<=182)):continue
                ranks=[]
                for b,(ref,(ti,te),masks) in layers.items():
                    m=next(measure(ref,[(0,0,masks[0][a]|masks[1][w])]))
                    ranks.append(min(m[2]-ti,2*(te-m[-1])))
                tested.append((n,a,w,*ranks))
        chosen=set()
        for group in (0,1,2):
            step=12 if d==41 else 5
            for low in range(0,520 if d==41 else 190,step):
                bucket=[r for r in tested if low<=r[0]<low+step]
                if bucket:chosen.add(max(bucket,key=lambda r:min(r[3:]) if group==2 else r[3+group])[:3])
        dest=ROOT/"mixed-part";dest.mkdir(exist_ok=True)
        for n,a,w in sorted(chosen):
            key=f"{d}-{len(candidates)}"
            av,ai,s=slots(ROOT/a);wv,wi,t=slots(ROOT/w)
            li=ai[s==0];wood=wi[t==1]
            p=dest/(key+".mesh");save(p,np.vstack([av,wv]),np.vstack([li,wood+len(av)]))
            p.with_suffix(".mat").write_bytes(bytes(len(li))+bytes([1])*len(wood))
            candidates.append(dict(d=d,name=p.relative_to(ROOT).as_posix(),leaf=a,wood=w,per_spray=n))
    table("mixed-candidates.csv",candidates)
    # Score all candidates on the three development views before adaptive fitting.
    from ScoreReusableOak import projected
    rows=[]
    for d in (41,100):
        for yaw in (132,90,180):
            for b,groups in regions.items():
                ids=np.concatenate([g[1] for g in groups])
                crop,(ti,te)=setup(b,d,yaw);ref=projected(ROOT/"spray.mesh",ids,d,yaw,crop);mask=np.isfinite(ref)
                for c in candidates:
                    if c["d"]!=d:continue
                    z=projected(ROOT/c["name"],ids,d,yaw,crop);occ=np.isfinite(z);shared=mask&occ
                    m=next(measure(mask,[(0,0,occ)]))
                    rows.append(dict(d=d,b=b,yaw=yaw,cut=c["name"],per_spray=c["per_spray"],tri=c["per_spray"]*len(ids),
                                     iou=m[2],edge=m[-1],ue_iou=ti,ue_edge=te,passed=m[2]>=ti and m[-1]<=te,
                                     p95_mm=np.percentile(abs(z[shared]-ref[shared])*1000,95)))
    table("mixed-screen.csv",rows)

if __name__=="__main__":run()
