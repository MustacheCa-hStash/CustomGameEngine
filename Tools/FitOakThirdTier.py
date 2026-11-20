"""Try a middle shared template between the two accepted projected-size tiers."""
import csv
import json
import math
import numpy as np
from PackageReusableOak import layout
from PackageOakAssembly import pixel_scale
from ScoreReusableOak import ROOT,projected,setup,measure,slots,table

def run():
    alltiers=[]
    for d in (41,100):
        if d==41:
            small=["mixed-part/41-10.mesh","epic-0.6/level04-paired.mesh","mixed-part/41-14.mesh"]
            mid=["mixed-part/41-27.mesh","mixed-part/41-28.mesh","mixed-part/41-29.mesh","mixed-part/41-32.mesh"]
            large=["epic-0.8/level07-paired.mesh","epic-0.6/level03.mesh","native-part/part-630.mesh"]
            thresholds=np.arange(59.,80.,1.);end=90
        else:
            small=["mixed-part/100-45.mesh","mixed-part/100-46.mesh","mixed-part/100-48.mesh"]
            mid=["mixed-part/100-55.mesh","mixed-part/100-56.mesh","mixed-part/100-60.mesh","epic-0.5/level04.mesh"]
            large=["epic-0.65/level06.mesh","epic-0.7/level07.mesh","epic-0.75/level08.mesh","native-part/part-252.mesh"]
            thresholds=np.arange(25.,28.61,.2);end=42
        params=[(a,b,c,i,j) for a in small for b in mid for c in large for i in range(len(thresholds))
                for j in range(i+1,len(thresholds))]
        costs={c:len(slots(ROOT/c)[1]) for c in set(small+mid+large)}
        scores={q:[0.,0.,1.] for q in params}
        pop=np.array([i.bit_count() for i in range(256)],np.uint8)
        for yaw in (132,270,0,90,180,45,225):
            for region,groups in layout().items():
                crop,(ti,te)=setup(region,d,yaw);groups=sorted(groups,key=lambda g:pixel_scale(g[2],d,yaw))
                ids=np.concatenate([g[1] for g in groups]);ref=np.isfinite(projected(ROOT/"spray.mesh",ids,d,yaw,crop))
                packed=np.packbits(ref);zero=np.zeros_like(packed)
                lim=np.searchsorted([pixel_scale(g[2],d,yaw) for g in groups],thresholds*.9999)
                ns=np.r_[0,np.cumsum([len(g[1]) for g in groups])]
                intervals={}
                for c in set(x for q in scores for x in q[:3]):
                    masks=[np.packbits(np.isfinite(projected(ROOT/c,g[1],d,yaw,crop))) for g in groups]
                    for s,t in {(0,lim[q[3]]) if q[0]==c else (lim[q[3]],lim[q[4]]) if q[1]==c else (lim[q[4]],len(groups))
                                for q in scores if c in q[:3]}:
                        intervals[c,s,t]=np.bitwise_or.reduce(masks[s:t],axis=0) if s<t else zero
                accepted={}
                memo={}
                for q,stats in scores.items():
                    a,b,c,i,j=q;s,t=lim[i],lim[j]
                    identity=(a if s else "",b if t>s else "",c if t<len(groups) else "",s,t)
                    if identity not in memo:
                        mask=intervals[a,0,s]|intervals[b,s,t]|intervals[c,t,len(groups)]
                        iou=pop[mask&packed].sum()/pop[mask|packed].sum();margin=iou-ti
                        if margin>=0:
                            occ=np.unpackbits(mask,count=ref.size).reshape(ref.shape).view(bool)
                            margin=min(margin,te-next(measure(ref,[(0,0,occ)]))[-1])
                        memo[identity]=margin
                    margin=memo[identity]
                    if margin<0:continue
                    if yaw==132:
                        tris=costs[a]*ns[s]+costs[b]*(ns[t]-ns[s])+costs[c]*(ns[-1]-ns[t])
                        ratio=tris/({(7,41):158190,(8,41):104766,(7,100):58823,(8,100):42272}[region,d])
                        stats[0]=max(stats[0],ratio);stats[1]+=ratio/2
                    stats[2]=min(stats[2],margin);accepted[q]=stats
                scores=accepted
                if not scores:break
            if not scores:break
        rows=sorted((dict(d=d,small=q[0],mid=q[1],large=q[2],a=round(thresholds[q[3]],3),b=round(thresholds[q[4]],3),
                          worst=s[0],mean=s[1],margin=s[2]) for q,s in scores.items()),key=lambda r:(r["worst"],r["mean"],-r["margin"]))
        prior=list(csv.DictReader((ROOT/f"frontier-{d}.csv").open()))
        duo=next(r for r in prior if float(r["margin"])>=.00035 and "native" not in r["cheap"]+r["fine"])
        tiers=[[float(duo["threshold"]),duo["cheap"]],[end,duo["fine"]]]
        if rows:
            table(f"three-tiers-{d}.csv",rows)
            best=next((r for r in rows if r["margin"]>=.00035 and "native" not in r["large"]),None)
            if best and best["worst"]<float(duo["worst_ratio"]):
                tiers=[[best["a"],best["small"]],[best["b"],best["mid"]],[end,best["large"]]]
        print(d,tiers,flush=True);alltiers=tiers+alltiers
    # The simplified meshes were only qualified up to the largest projected
    # scale in these cameras. Use the shared source outside that envelope.
    biggest = max(pixel_scale(node,41,yaw) for groups in layout().values()
                  for _,_,node in groups for yaw in (132,90,180,45,225,0,270))
    alltiers[-1][0] = math.ceil(biggest)
    alltiers.append([95,"spray.mesh"])
    (ROOT/"policy.json").write_text(json.dumps(alltiers,indent=2))

if __name__=="__main__":run()
