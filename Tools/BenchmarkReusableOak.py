"""Time actual indexed instanced draws and calibrated stock and native controls."""
import argparse
import csv
import json
import os
import shutil
from statistics import median
from PackageReusableOak import layout,package
from ScoreReusableOak import slots,ue_images
from ReplayOakAssembly import BUILD,OUT,TREE,PREVIOUS,viewer_assets,capture,quality,read_mask,reference
from ReusableOakSprays import ROOT,table

ALL_YAWS=(132,90,180,45,225,0,270)

def target(b,d,yaw):
    p,f=ue_images(b,d,yaw)
    return quality(p,read_mask(f,True),True)

def photo(view,label,b,d,yaw=132,budget=1):
    path,row=capture(view,"reuse-"+label,d,yaw,budget,mixed=True)
    p,_=capture(ROOT/f"ref-{b}",f"reuse-ref-{b}",d,yaw,1,mixed=True)
    q,e=quality(path,read_mask(p));u,w=target(b,d,yaw)
    return dict(b=b,d=d,yaw=yaw,cut=label,budget=budget,tri=row["triangles"]-5,iou=q,edge=e,ue_iou=u,ue_edge=w,passed=q>=u and e<=w)

def grid():
    rows=[];chosen={}
    for b in layout():
        view=package(f"grid-{b}",only=b)
        ref=package(f"ref-{b}",only=b,source=True)
        for d in (41,100):
            _,source=capture(ref,f"reuse-ref-{b}",d,132,1,mixed=True)
            assert source["triangles"]-5==2454*sum(len(g[1]) for g in layout()[b]),source
            def evaluate(budget):
                result=photo(view,f"grid-{b}",b,d,132,budget)
                rows.append(result)
                return result
            sweep=[evaluate(budget) for budget in (0.20,.32,.44,.56,.70,.85,1,1.20,1.5,2.,3.,4.)]
            for _ in range(5):
                lo=max((r["budget"] for r in sweep if r["passed"]),default=.05)
                hi=min((r["budget"] for r in sweep if r["budget"]>lo),default=lo)
                if hi==lo:break
                sweep.append(evaluate(round((lo+hi)/2,5)))
            accepted=None
            for r in sorted((x for x in sweep if x["passed"]),key=lambda x:(x["tri"],-x["budget"])):
                hold=[photo(view,f"grid-{b}",b,d,y,r["budget"]) for y in ALL_YAWS[1:]]
                rows.extend(hold)
                if all(q["passed"] for q in hold):
                    accepted=r;break
            if not accepted:
                for budget in (.16,.10,.05):
                    allrows=[photo(view,f"grid-{b}",b,d,y,budget) for y in ALL_YAWS];rows+=allrows
                    if all(q["passed"] for q in allrows):accepted=allrows[0];break
            assert accepted,(b,d)
            chosen[f"{b}-{d}"]=accepted
            table("grid-masks.csv",rows)
            (ROOT/"grid-chosen.json").write_text(json.dumps(chosen,indent=2))
            print("grid",b,d,accepted,flush=True)

def finalists():
    policy=json.loads((ROOT/"policy.json").read_text())
    rows=[]
    for b,groups in layout().items():
        view=package(f"pilot-{b}",policy,only=b)
        for d in (41,100):
            for yaw in ALL_YAWS:
                rows.append(photo(view,f"pilot-{b}",b,d,yaw))
            live=f"live{b}-{d}.mesh"
            replay=package(f"replay-{b}-{d}",only=b,live=live)
            path,row=capture(replay,f"reuse-replay-{b}",d,132,1,mixed=True)
            ue,_=ue_images(b,d,132)
            print("native replay",b,d,row["triangles"]-5,quality(path,read_mask(ue,True))[0],flush=True)
            assert row["triangles"]-5==len(slots(ROOT/live)[1])
        table("pilot-masks.csv",rows)

def refine():
    rows=list(csv.DictReader((ROOT/"grid-masks.csv").open()))
    chosen=json.loads((ROOT/"grid-chosen.json").read_text())
    for b in layout():
        for d in (41,100):
            key=f"{b}-{d}";best=chosen[key];lo=best["budget"]
            # A new validation yaw can invalidate a previously accepted bracket.
            for x in [lo,*sorted({float(r["budget"]) for r in rows
                                   if int(r["b"])==b and int(r["d"])==d and float(r["budget"])<lo},reverse=True)]:
                candidate=[photo(ROOT/f"grid-{b}",f"grid-{b}",b,d,y,x) for y in ALL_YAWS]
                rows.extend(candidate)
                if all(r["passed"] for r in candidate):
                    best=candidate[0];lo=x;break
            else:raise RuntimeError(f"No matching stock grid for {key}")
            failed=[float(r["budget"]) for r in rows if int(r["b"])==b and int(r["d"])==d
                    and str(r["passed"])=="False" and float(r["budget"])>lo]
            hi=min(failed,default=lo)
            for _ in range(4):
                x=round((lo+hi)/2,5)
                if x in (lo,hi):break
                trial=[]
                for yaw in ALL_YAWS:
                    trial.append(photo(ROOT/f"grid-{b}",f"grid-{b}",b,d,yaw,x))
                    if not trial[-1]["passed"]:break
                rows.extend(trial)
                if all(r["passed"] for r in trial):
                    lo=x
                    if trial[0]["tri"]<=best["tri"]:best=trial[0]
                else:hi=x
            chosen[key]=best
    table("grid-masks.csv",rows);(ROOT/"grid-chosen.json").write_text(json.dumps(chosen,indent=2))

def whole():
    policy=json.loads((ROOT/"policy.json").read_text())
    production=viewer_assets("reusable/production",PREVIOUS/"current")
    shutil.copyfile(OUT/"production-new/assets"/TREE,production/"assets"/TREE)
    grid=package("whole-grid");pilot=package("pilot",policy)
    rows=[];chosen={}
    for d in (41,100):
        targetrow=next(r for r in csv.DictReader((BUILD/"oak-gpu-selection/pixel-edge-audit.csv").open())
                      if r["renderer"]=="Nanite" and int(r["distance_m"])==d)
        ti=float(targetrow["iou"]); te=float(targetrow["edge_fraction_beyond_1px"]); ref=reference(d,whole=True)
        for name,view in (("production",production),("whole-grid",grid),("pilot",pilot)):
            def check(budget):
                path,row=capture(view,"reuse-whole-"+name,d,132,budget)
                q,e=quality(path,ref)
                r=dict(d=d,cut=name,budget=budget,tri=row["triangles"],iou=q,edge=e,passed=q>=ti and e<=te)
                rows.append(r);return r
            sweep=[check(x) for x in ((.85,.98,1.02211,1.05,1.10) if d==41 else (.38,.45,.49018,.51,.55))]
            if not any(q["passed"] for q in sweep):
                sweep.extend(check(x) for x in (.28,.18,.10))
            for _ in range(6):
                lo=max(r["budget"] for r in sweep if r["passed"])
                hi=min((r["budget"] for r in sweep if r["budget"]>lo),default=lo)
                if hi==lo:break
                sweep.append(check(round((lo+hi)/2,5)))
            chosen[f"{name}-{d}"]=min((r for r in sweep if r["passed"]),key=lambda r:r["tri"])
        common=min(chosen[f"{n}-{d}"]["budget"] for n in ("production","whole-grid","pilot"))
        for n,v in (("production",production),("whole-grid",grid),("pilot",pilot)):
            p,r=capture(v,"reuse-whole-"+n,d,132,common);q,e=quality(p,ref)
            chosen[f"common-{n}-{d}"]=dict(d=d,cut=n,budget=common,tri=r["triangles"],iou=q,edge=e,passed=q>=ti and e<=te)
        table("crown-masks.csv",rows);(ROOT/"crown-chosen.json").write_text(json.dumps(chosen,indent=2))

def validate():
    """Independent camera poses evaluated after the recipe and grid budgets were frozen."""
    settings=json.loads((ROOT/"grid-chosen.json").read_text())
    rows=[]
    for b in layout():
        for d in (41,100):
            for yaw in (68,315,302,330):
                for label,budget in [(f"pilot-{b}",1),(f"grid-{b}",settings[f"{b}-{d}"]["budget"])]:
                    rows.append(photo(ROOT/label,label,b,d,yaw,budget))
    table("frozen-validation.csv",rows)

def timing():
    grids=json.loads((ROOT/"grid-chosen.json").read_text());crown=json.loads((ROOT/"crown-chosen.json").read_text())
    series=os.environ.get("OAK_TIMING_SERIES","reuse-shaded")
    rows=[]
    for d in (41,100):
        cases=[]
        for b in layout():
            cases.extend([(f"pilot-{b}",1,True,b),(f"grid-{b}",grids[f"{b}-{d}"]["budget"],True,b),
                          (f"replay-{b}-{d}",1,True,b)])
        for n in ("production","whole-grid","pilot"):
            cases.append((n,crown[f"{n}-{d}"]["budget"],False,0))
            common=crown[f"common-{n}-{d}"]["budget"]
            if common!=cases[-1][1]:cases.append((n,common,False,0))
        for run in range(4):
            for label,budget,mixed,b in cases if run%2==0 else reversed(cases):
                viewer=viewer_assets("reusable/"+label,PREVIOUS/"current")
                _,r=capture(viewer,f"{series}-{label}-{run}",d,132,budget,True,mixed=mixed)
                rows.append(dict(d=d,b=b,cut=label,budget=budget,run=run,tri=r["triangles"]-(5 if b else 0),ms=r["ms"]))
            table("timings.csv",rows)

def summarize():
    """Compare shaded GPU medians at the silhouette-qualified settings."""
    timings=list(csv.DictReader((ROOT/"timings.csv").open()))
    for filename,count in [("pilot-masks.csv",28),("frozen-validation.csv",32)]:
        with (ROOT/filename).open() as stream:
            masks=list(csv.DictReader(stream))
        if len(masks)!=count or any(row["passed"]!="True" for row in masks):
            raise ValueError(f"Incomplete silhouette match in {filename}")
    crown=json.loads((ROOT/"crown-chosen.json").read_text())
    stock=json.loads((ROOT/"grid-chosen.json").read_text())
    native_counts=list(csv.DictReader((BUILD/"oak-triangles-matched/unreal-counts.csv").open()))
    profiles=BUILD/"unreal-nanite-oak/Saved/Profiling/CSV"
    def camera(label,d,budget):
        samples=[r for r in timings if r["cut"]==label and int(r["d"])==d and float(r["budget"])==budget]
        assert len(samples)==4 and len({r["tri"] for r in samples})==1
        return int(samples[0]["tri"]),median(float(r["ms"]) for r in samples)
    rows=[]
    for b in (7,8,0):
        for d in (41,100):
            if b:
                grid=camera(f"grid-{b}",d,stock[f"{b}-{d}"]["budget"])
                trial=camera(f"pilot-{b}",d,1)
                native,replay=camera(f"replay-{b}-{d}",d,1)
            else:
                grid=camera("production",d,crown[f"production-{d}"]["budget"])
                trial=camera("pilot",d,crown[f"pilot-{d}"]["budget"])
                native=int(next(r["triangles"] for r in native_counts if int(r["distance"])==d and r["condition"]=="frustum" and r["bricks"]=="0"))
                replay=None
            tag=f"b{b}" if b else "crown"
            medians=[]
            for run in (1,2):
                with (profiles/f"spray-final-{tag}-{d}-r{run}.csv").open(encoding="utf-8-sig") as stream:
                    values=[float(r["GPU/NaniteVisBuffer"])+float(r["GPU/NaniteBasePass"]) for r in csv.DictReader(stream)
                            if r.get("GPU/NaniteVisBuffer") and r["GPU/NaniteVisBuffer"].replace(".","",1).isdigit()]
                assert len(values)==65
                medians.append(median(values[8:-5]))
            rows.append(dict(region=tag,distance_m=d,stock_tri=grid[0],stock_ms=grid[1],reusable_tri=trial[0],
                             reusable_ms=trial[1],nanite_tri=native,nanite_ms_min=min(medians),nanite_ms_max=max(medians),replay_ms=replay))
    table("matched-costs.csv",rows)

if __name__=="__main__":
    tasks={"grid":grid,"refine":refine,"finalists":finalists,"whole":whole,"validate":validate,"timing":timing,"summarize":summarize}
    parser=argparse.ArgumentParser();parser.add_argument("task",choices=tasks)
    tasks[parser.parse_args().task]()
