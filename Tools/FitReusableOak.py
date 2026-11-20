"""Fit one projected-size rule across both boughs using reusable paired material ranges."""
import csv
import argparse
import json
import numpy as np
from PackageReusableOak import layout
from PackageOakAssembly import pixel_scale
from ScoreReusableOak import ROOT,measure,projected,setup,slots,table

def fit(yaws=(132,90,180),margin=.0002):
    screen=list(csv.DictReader((ROOT/"template-screen.csv").open()))
    if (ROOT/"mixed-screen.csv").exists():
        screen+=list(csv.DictReader((ROOT/"mixed-screen.csv").open()))
    regions=layout(); policies=[]
    pop=np.array([i.bit_count() for i in range(256)],np.uint8)
    for d in (41,100):
        rows=[r for r in screen if int(r["d"])==d and int(r["yaw"])==132]
        costs={r["cut"]:int(r["per_spray"]) for r in rows}
        cuts=[]
        bins=([210,270,315,350,380,405,430,465,510,555,650,720] if d==41 else [75,95,110,126,142,155,171,188,209,230,255])
        for lo,hi in zip(bins,bins[1:]):
            for b in regions:
                r=[s for s in rows if s["b"]==str(b) and lo<=int(s["per_spray"])<hi and not s["cut"].startswith("native")]
                if r:cuts.append(max(r,key=lambda s:min(float(s["iou"])-float(s["ue_iou"]),3*(float(s["ue_edge"])-float(s["edge"]))))["cut"])
        cuts=sorted(set(cuts)|{r["cut"] for r in rows if r["cut"].startswith("native")})
        # Include economical uniform cuts that already passed every development yaw.
        for b in regions:
            uniform={}
            for r in screen:
                if int(r["d"])==d and int(r["b"])==b:uniform.setdefault(r["cut"],[]).append(r)
            cuts.extend(k for _,k in sorted((costs[k],k) for k,v in uniform.items()
                if len(v)==3 and all(x["passed"]=="True" for x in v))[:5])
        if d==41:cuts+=["epic-0.8/level10-paired.mesh","epic-0.75/level06-paired.mesh"]
        cuts=sorted(set(cuts))
        thresholds=np.arange(57,81,.75) if d==41 else np.arange(24.8,28.6,.15)
        cases=[]
        for yaw in yaws:
            for b,groups in regions.items():
                crop,(ti,te)=setup(b,d,yaw)
                ids=np.concatenate([g[1] for g in groups])
                ref=np.isfinite(projected(ROOT/"spray.mesh",ids,d,yaw,crop))
                packed=np.packbits(ref)
                order=sorted(groups,key=lambda g:pixel_scale(g[2],d,yaw))
                sc=np.array([pixel_scale(g[2],d,yaw) for g in order])
                splits=np.searchsorted(sc,thresholds*.9999)
                low={};high={}
                for c in cuts:
                    masks=[np.packbits(np.isfinite(projected(ROOT/c,g[1],d,yaw,crop))) for g in order]
                    pre=np.bitwise_or.accumulate(masks,axis=0);post=np.bitwise_or.accumulate(masks[::-1],axis=0)
                    zero=np.zeros_like(packed)
                    low[c]=[pre[s-1] if s else zero for s in splits]
                    high[c]=[post[len(order)-s-1] if s<len(order) else zero for s in splits]
                n=np.array([len(g[1]) for g in order]);counts=np.r_[0,n.cumsum()][splits]
                cases.append((b,yaw,ref,packed,ti,te,low,high,counts,int(n.sum())))
        valid=[]
        limits={(7,41):158190,(8,41):104766,(7,100):58823,(8,100):42272}
        for a in cuts:
            for f in cuts:
                if costs[a]>costs[f]:continue
                for j,threshold in enumerate(thresholds):
                    ratios=[];margins=[]
                    for b,yaw,ref,packed,ti,te,low,high,counts,total in cases:
                        m=low[a][j]|high[f][j]
                        iou=pop[m&packed].sum()/pop[m|packed].sum()
                        if iou<ti:break
                        occ=np.unpackbits(m,count=ref.size).reshape(ref.shape).view(bool)
                        e=next(measure(ref,[(0,0,occ)]))[-1]
                        if e>te:break
                        margins.append(min(iou-ti,te-e))
                        if yaw==132:ratios.append((costs[a]*counts[j]+costs[f]*(total-counts[j]))/limits[b,d])
                    else:
                        valid.append(dict(d=d,cheap=a,fine=f,threshold=round(float(threshold),4),
                                          worst_ratio=max(ratios),mean_ratio=np.mean(ratios),margin=min(margins)))
        valid.sort(key=lambda q:(q["worst_ratio"],q["mean_ratio"],-q["margin"]))
        assert valid
        table(f"frontier-{d}.csv",valid)
        # Keep a modest rasterization margin for the final hardware validation.
        robust=[r for r in valid if r["margin"]>=margin and "native-part" not in r["cheap"]+r["fine"]]
        chosen=next(iter(robust),valid[0])
        policies.append(chosen)
        print(d,chosen,"best unrestricted",valid[0],flush=True)
    near,far=policies
    tiers=[[far["threshold"],far["cheap"]],[42,far["fine"]],[near["threshold"],near["cheap"]],[85,near["fine"]]]
    (ROOT/"policy.json").write_text(json.dumps(tiers,indent=2))

if __name__=="__main__":
    args=argparse.ArgumentParser();args.add_argument("--yaws",nargs="+",type=int,default=[132,90,180])
    args.add_argument("--margin",type=float,default=.0002)
    options=args.parse_args();fit(options.yaws,options.margin)
