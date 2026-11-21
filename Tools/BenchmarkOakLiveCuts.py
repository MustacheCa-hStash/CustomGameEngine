"""Run and verify the live UE, batched UE, and per-spray D3D selections."""
import argparse
import csv
import hashlib
import json
import os
import shutil
import subprocess
import uuid
from functools import lru_cache
from pathlib import Path
from statistics import median

import numpy as np
from PackOakLiveCuts import OUT
from ReplayOakAssembly import OUT as ASSEMBLY, PREVIOUS, TREE, UE, viewer_assets, measure, read_mask
from ReusableOakSprays import ROOT
from ScoreReusableOak import ue_images
from DecodeOakBoughVoxels import write_csv

CAP=OUT/"captures"
CAMERAS=(132,90,180,45,225,0,270,68,315,302,330,23,197)
CROWN_BUDGET={41:1.03649,100:.49018}
@lru_cache(None)
def viewer(subject,reference=False,production=False,ordered=False):
    source=ROOT/("production" if production else ("ref-" if reference else "pilot-")+subject if subject!="crown" else "pilot")
    if not (source/"assets"/TREE).exists():
        if production:
            source=viewer_assets("reusable/production",PREVIOUS/"current")
            shutil.copy2(ASSEMBLY/"production-new"/"assets"/TREE,source/"assets"/TREE)
        else:
            from PackageReusableOak import package
            policy=json.loads((ROOT/"policy.json").read_text())
            source=package(source.name,policy,only=None if subject=="crown" else int(subject),
                           source=reference and subject!="crown")
    dest=viewer_assets(str(OUT/("viewer-"+subject+("-ref" if reference else "-production" if production else "-ordered" if ordered else ""))),source)
    target=dest/"assets"/TREE
    original=OUT/"cache-trunk.treelod" if ordered else source/"assets"/TREE
    if not target.exists() or not os.path.samefile(target,original):
        tmp=target.with_name(uuid.uuid4().hex)
        os.link(original,tmp)
        tmp.replace(target)
    return dest/"CustomGameEngine.exe"

def native_paths(subject,d,y):
    if subject=="crown" or y in (132,23,197):
        stem=f"selector-{subject}-{d}-{y}"
        return UE/(stem+".png"),UE/(stem+"-fine.png")
    return ue_images(int(subject),d,y)

def command(subject,arm,d,y,limit=1.,wind=0,probe_gi="off"):
    ordered=arm.endswith("-ordered")
    arm=arm.removesuffix("-ordered")
    ref=arm=="source"
    exe=viewer(subject,ref,arm=="production",ordered)
    args=["--scene","forked-oak","--camera","overview","--width","3840","--height","2160","--msaa","1",
          "--distance",str(d),"--pitch-degrees","-5.46" if d==41 else "-2.235","--yaw-degrees",str(y),
          "--wind-strength",str(wind),"--wind-time","0","--lighting","combined",
          "--cull-spray-wood-backfaces"]
    if probe_gi is not None: args.extend(["--probe-gi",probe_gi])
    if ref and subject=="crown":
        # UE imported the 1,504-leaf / 950-wood cut, which is D3D fixed LOD 4.
        args+=["--lod","4"]
    else:
        args+=["--lod","auto","--spray-mode","hierarchy","--tree-submit","direct","--spray-culling","on",
               "--spray-local-lod","on","--spray-pixel-error",str((CROWN_BUDGET.get(d,1) if subject=="crown" else 1)*(limit if arm in ("current","production") else 1))]
    if arm not in ("current","source","production"):
        algo=arm.split("-")[0]
        region="both" if arm.endswith("-insert") else subject
        args+=["--live-cut",str(OUT/f"{algo}-{region}.oakcut"),"--live-cut-pixel-error",str(limit)]
        if "raw" in arm:args+=["--live-cut-unbatched"]
        if "compact" in arm or arm in ("hybrid","canopy","canopy1"):args+=["--live-cut-pack-assembly"]
    return exe,args

def shot(subject,arm,d,y,repeat=None,limit=1.,extra=()):
    CAP.mkdir(exist_ok=True)
    exe,args=command(subject,arm,d,y,limit)
    args+=["--timing-samples","61" if repeat is not None else "3",*extra]
    if repeat is None:args.append("--geometry-mask")
    paths=[exe,exe.parent/"assets"/TREE]+[Path(args[args.index("--live-cut")+1])] if "--live-cut" in args else [exe,exe.parent/"assets"/TREE]
    signature=repr((args,[(p.stat().st_size,p.stat().st_mtime_ns) for p in paths],repeat))
    stamp=hashlib.sha256(signature.encode()).hexdigest()[:12]
    path=CAP/f"{subject}-{arm}-{d}-{y}-{stamp}.png"
    if not path.exists():
        temp=CAP/(uuid.uuid4().hex+".png")
        with path.with_suffix(".log").open("w") as f:
            subprocess.run([exe,"--capture",temp,*args],check=True,stdout=f,stderr=f,timeout=160)
        temp.with_suffix(".timings.csv").replace(path.with_suffix(".timings.csv"));temp.replace(path)
    with path.with_suffix(".timings.csv").open() as f:rows=list(csv.DictReader(f))
    r=rows[0]
    data=dict(tris=sum(int(r[k]) for k in ("camera_leaf_triangles","camera_spray_wood_triangles","camera_tree_trunk_triangles"))-(5 if subject!="crown" else 0),
        draws=int(r["camera_draws"]),shadow_tris=int(r["shadow_triangles"]))
    for k in ("cpu_ms","gpu_camera_ms","gpu_shadow_ms","gpu_frame_ms","camera_selection_cpu_ms","camera_tree_submit_ms","camera_tree_draws","camera_leaf_triangles","camera_spray_wood_triangles","camera_tree_trunk_triangles"):
        data[k]=median(float(t[k]) for t in rows)
    for k in ("camera_mesh_vs_invocations","camera_mesh_ps_invocations"):
        if k in r:data[k]=int(r[k])
    if "fenced_frame_ms" in r:data["fenced_frame_ms"]=median(float(t["fenced_frame_ms"]) for t in rows)
    if subject=="crown" and arm=="source":
        assert (data["camera_leaf_triangles"],data["camera_spray_wood_triangles"])==(4730*1504,4730*950),data
    data["capture"]=str(path.relative_to(OUT))
    return path,data

def metrics(path,ref,unreal=False):
    q=next(measure(ref,[(0,0,read_mask(path,unreal))]))
    return float(q[2]),float(q[-1])

@lru_cache(maxsize=2)
def refs(subject,d,y):
    p,f=native_paths(subject,d,y)
    native,fine=read_mask(p,True),read_mask(f,True)
    gate=next(measure(fine,[(0,0,native)]))
    return native,fine,(float(gate[2]),float(gate[-1]))

def quality_row(subject,d,y,arm,limit):
    native,fine,(ti,te)=refs(subject,d,y)
    path,c=shot(subject,arm,d,y,limit=limit)
    iou,e=metrics(path,fine)
    local=read_mask(shot(subject,"source",d,y)[0])
    li,le=metrics(path,local)
    mask=read_mask(path)
    fid=np.count_nonzero(mask&native)/np.count_nonzero(mask|native)
    return dict(subject=subject,d=d,yaw=y,arm=arm,limit=limit,**c,iou=iou,edge=e,
                own_iou=li,own_edge=le,gate_iou=ti,gate_edge=te,common_pass=iou>=ti and e<=te,
                own_pass=li>=ti and le<=te,native_agreement=fid)

def calibrate():
    records=[]
    for subject in ("crown","7","8"):
        for d in (100,41):
            arms=["current","sprays","native","native-compact"]
            if subject=="crown":arms+=["production","native-insert","sprays-insert"]
            for arm in arms:
                row=quality_row(subject,d,132,arm,1)
                records.append(row)
                print(subject,d,arm,row["tris"],row["common_pass"],row["own_pass"],round(row["iou"],5),round(row["edge"],5),flush=True)
                if arm in ("native-compact","native-insert","sprays-insert"):continue
                if row["common_pass"] and row["own_pass"]:continue
                for limit in (.95,.85,.70,.50,.32):
                    row=quality_row(subject,d,132,arm,limit)
                    records.append(row)
                    if row["common_pass"] and row["own_pass"]:break
            write_csv(OUT/"calibration.csv",records)

def chosen_path():
    return OUT/"settings.json"

def freeze():
    rows=list(csv.DictReader((OUT/"calibration.csv").open()))
    chosen={}
    refinement=[]
    for subject in ("crown","7","8"):
        for d in (41,100):
            for arm in (["sprays","current","native","production"] if subject=="crown" else ["sprays","current","native"]):
                seed=[r for r in rows if (r["subject"],r["d"],r["arm"])==(subject,str(d),arm)]
                accepted=lambda r:r["common_pass"]==r["own_pass"]=="True"
                lo=max(float(r["limit"]) for r in seed if accepted(r))
                hi=min((float(r["limit"]) for r in seed if not accepted(r) and float(r["limit"])>lo),default=lo)
                for j in range(5):
                    if hi==lo:break
                    x=round((hi+lo)/2,6)
                    row=quality_row(subject,d,132,arm,x)
                    refinement.append(row)
                    if row["common_pass"] and row["own_pass"]:lo=x
                    else:hi=x
                chosen[f"{subject}/{d}/{arm}"]=lo
    chosen_path().write_text(json.dumps(chosen,indent=2))
    write_csv(OUT/"refinement.csv",refinement)

def settings():
    return json.loads(chosen_path().read_text())

def verify():
    limits=settings()
    rows=[]
    for subject in ("crown","7","8"):
        for d in (41,100):
            for y in ((132,23,197) if subject=="crown" else CAMERAS):
                native=quality_row(subject,d,y,"native",1)
                rows.append(native)
                for arm in (["production","current","sprays","native-compact"] if subject=="crown" else ["current","sprays","native-compact"]):
                    x=limits[f"{subject}/{d}/{arm.split('-')[0]}"]
                    rows.append(quality_row(subject,d,y,arm,x))
                if y in (132,23,197):
                    path,_=shot(subject,"native",d,y)
                    compact,c=shot(subject,"native-compact",d,y)
                    assert native["tris"]==c["tris"] and np.array_equal(read_mask(path),read_mask(compact))
                    if y==132:
                        raw,c=shot(subject,"native-raw",d,y)
                        assert native["tris"]==c["tris"] and np.array_equal(read_mask(path),read_mask(raw))
                        assert native["tris"]=={"7":{41:158190,100:58823},"8":{41:104766,100:42272},
                            "crown":{41:2513534,100:1073068}}[subject][d]
                write_csv(OUT/"verification.csv",rows)
            print("verified",subject,d,flush=True)

def timing():
    limits=settings()
    rows=[]
    for subject in ("crown","7","8"):
        for d in (41,100):
            arms=[(a,limits[f"{subject}/{d}/{a}"]) for a in (["production","current","sprays"] if subject=="crown" else ["current","sprays"])]
            arms += [(a,1) for a in ("native-raw","native","native-compact")]
            arms += [("native-compact",limits[f"{subject}/{d}/native"])]
            for repeat in range(4):
                for arm,x in (arms if repeat%2==0 else reversed(arms)):
                    p,c=shot(subject,arm,d,132,repeat,x)
                    rows.append(dict(subject=subject,d=d,arm=arm,limit=x,repeat=repeat,**c))
                shadow={r["shadow_tris"] for r in rows if r["subject"]==subject and r["d"]==d and r["arm"]!="production"}
                assert len(shadow)==1,shadow
                write_csv(OUT/"timing.csv",rows)
            print("timed",subject,d,flush=True)

def hybrid():
    # Slot policy and the 1 px setting are frozen before checking other views.
    rows=[];timings=[]
    for subject in ("crown","7","8"):
        for d in (41,100):
            for y in ((132,23,197) if subject=="crown" else CAMERAS):
                row=quality_row(subject,d,y,"hybrid",1)
                rows.append(row)
                write_csv(OUT/"hybrid-quality.csv",rows)
            for repeat in range(4):
                for arm in (("hybrid","sprays") if repeat%2==0 else ("sprays","hybrid")):
                    p,c=shot(subject,arm,d,132,repeat+10)
                    timings.append(dict(subject=subject,d=d,arm=arm,repeat=repeat,limit=1,**c))
            write_csv(OUT/"hybrid-timing.csv",timings)
            print("hybrid",subject,d,flush=True)

def screen():
    rows=[]
    for d in (41,100):
        for y in CAMERAS:
            for arm in ("sprays","hybrid"):
                rows.append(quality_row("crown",d,y,arm,1))
            write_csv(OUT/"crown-screen.csv",rows)

def frame_time():
    rows=[]
    for d in (41,100):
        arms=["current","tiered","native","native-compact","canopy1","production"]
        for rep in range(4):
            for arm in (arms if rep%2==0 else reversed(arms)):
                x=settings().get(f"crown/{d}/{arm}",1) if arm in ("current","production") else 1
                _,c=shot("crown",arm,d,132,50+rep,x)
                assert c["fenced_frame_ms"]>0,c
                rows.append(dict(subject="crown",d=d,arm=arm,repeat=rep,limit=x,**c))
            write_csv(OUT/"fenced-timing.csv",rows)

def counters():
    from PIL import Image
    rows=[];agreement=[]
    guards=json.loads((OUT/"guard.json").read_text()) if (OUT/"guard.json").exists() else {}
    for subject,d in (("crown",41),("crown",100),("7",100),("8",41)):
        native_image=None
        arms=[("current",settings()[f"{subject}/{d}/current"]),("sprays",1),("native",1),("native-compact",1)]
        arms.extend((a,x) for a,x in guards.items() if (a,x) not in arms)
        for arm,x in arms:
            p,c=shot(subject,arm,d,132,99,x,extra=("--camera-pipeline-stats",))
            assert c["camera_mesh_vs_invocations"] and c["camera_mesh_ps_invocations"],c
            rows.append(dict(subject=subject,d=d,arm=arm,limit=x,**c))
            if arm in ("native","native-compact"):
                with Image.open(p) as im:rgb=np.asarray(im.convert("RGB"),dtype=np.int16)
                if native_image is None:native_image=rgb
                else:
                    delta=abs(rgb-native_image).max(axis=2)
                    agreement.append(dict(subject=subject,d=d,differing_pixels=int(np.count_nonzero(delta)),
                                          pixels_over_one_code=int(np.count_nonzero(delta>1)),max_code=int(delta.max())))
    write_csv(OUT/"pipeline-counters.csv",rows)
    write_csv(OUT/"shaded-agreement.csv",agreement)

def order_test():
    import struct
    from PIL import Image
    from ReplayOakAssembly import Hierarchy
    # Change only the order of triangles in two baked trunk draw ranges.
    # The canopy and every vertex attribute are byte-identical.
    old,new=(Hierarchy(p.read_bytes()) for p in (ROOT/"pilot"/"assets"/TREE,OUT/"cache-trunk.treelod"))
    assert (old.header,old.order,old.parts[:2],old.tail)==(new.header,new.order,new.parts[:2],new.tail)
    assert old.parts[2][:3]==new.parts[2][:3]
    for offset in range(0,len(old.parts[2][1]),56):
        first,n=struct.unpack_from("<II",old.parts[2][1],offset+24)
        if n:
            oriented=[]
            for hierarchy in (old,new):
                faces=np.frombuffer(hierarchy.parts[2][3],"<u4",count=n,offset=4*first).reshape(-1,3)
                oriented.append(faces[np.lexsort(faces.T[::-1])])
            assert np.array_equal(*oriented)
    rows=[];quality=[];counts=[]
    for d in (41,100):
        for a,x in [("sprays",1)]:
            arms=[a] if a=="native-compact" else [a,a+"-ordered"]
            for rep in range(4):
                for arm in (arms if rep%2==0 else reversed(arms)):
                    p,c=shot("crown",arm,d,132,30+rep,x)
                    rows.append(dict(d=d,arm=arm,limit=x,repeat=rep,**c))
                    if rep==0 and len(arms)>1:
                        with Image.open(p) as im:rgb=np.asarray(im.convert("RGB"),dtype=np.int16)
                        if arm==a:original=rgb;original_tris=c["tris"]
                        else:
                            assert c["tris"]==original_tris
                            delta=abs(rgb-original).max(axis=2)
                            quality.append(dict(d=d,arm=a,limit=x,shaded_pixels_over_one=int(np.count_nonzero(delta>1)),
                                                max_code=int(delta.max())))
            for arm in arms:
                p,c=shot("crown",arm,d,132,99,x,extra=("--camera-pipeline-stats",))
                counts.append(dict(d=d,arm=arm,limit=x,**c))
        write_csv(OUT/"order-timing.csv",rows)
        write_csv(OUT/"order-shaded.csv",quality)
        write_csv(OUT/"order-counters.csv",counts)

def canopy(quality_only=False,timing_only=False):
    from PackOakLiveCuts import pack_finalist
    margin=json.loads((OUT/"guard.json").read_text())["sprays"]
    if not (quality_only or timing_only):
        for arm in ("canopy","canopy1"):
            pack_finalist(arm,"crown",margin)
    quality=[];timed=[]
    for d in (41,100):
        if not timing_only:
            for y in CAMERAS:
                for arm in ("canopy","canopy1"):
                    quality.append(quality_row("crown",d,y,arm,1))
            write_csv(OUT/"canopy-quality.csv",quality)
        if quality_only:continue
        for rep in range(4):
            arms=["canopy","canopy1","tiered","native-compact"]
            for arm in (arms if rep%2==0 else reversed(arms)):
                _,c=shot("crown",arm,d,132,40+rep)
                timed.append(dict(d=d,arm=arm,repeat=rep,**c))
        write_csv(OUT/"canopy-timing.csv",timed)

def guard():
    # Pick one error margin per representation on the seven historical fitting
    # angles. Keep the six remaining angles out of the selection loop.
    rows=[];chosen={}
    for arm in ("sprays","hybrid"):
        def test(x,angles,phase,stop):
            passed=True
            for subject in ("crown","7","8"):
                for d in (41,100):
                    for y in angles:
                        r=quality_row(subject,d,y,arm,x)
                        rows.append(dict(phase=phase,**r))
                        passed &= r["common_pass"] and r["own_pass"]
                        if stop and not passed:return False
            return passed
        for x in (1,.98,.95,.90,.85,.8):
            if test(x,CAMERAS[:7],"fit",True):break
        else:
            print("No qualifying fitting margin for",arm,flush=True)
            write_csv(OUT/"guard.csv",rows)
            continue
        chosen[arm]=x
        print("frozen",arm,x,flush=True)
        test(x,CAMERAS[7:],"validation",False)
        write_csv(OUT/"guard.csv",rows)
    if "sprays" in chosen:
        # The first two baked projection tiers cover the 100 m cameras. Keep
        # their original bounds and apply the fitted margin to the finer tiers.
        from PackOakLiveCuts import pack_finalist
        arm="tiered"
        for subject in ("crown","7","8"):
            pack_finalist(arm,subject,chosen["sprays"])
        chosen[arm]=1
        test(1,CAMERAS[:7],"fit",False)
        test(1,CAMERAS[7:],"validation",False)
        write_csv(OUT/"guard.csv",rows)
    (OUT/"guard.json").write_text(json.dumps(chosen,indent=2))
    timings=[]
    for subject in ("crown","7","8"):
        for d in (41,100):
            for rep in range(4):
                arms=[*chosen,"native-compact","current"]+(["production"] if subject=="crown" else [])
                for arm in (arms if rep%2==0 else reversed(arms)):
                    x=chosen.get(arm,1 if arm=="native-compact" else settings().get(f"{subject}/{d}/{arm}",1))
                    _,c=shot(subject,arm,d,132,20+rep,x)
                    timings.append(dict(subject=subject,d=d,arm=arm,limit=x,repeat=rep,**c))
            write_csv(OUT/"guard-timing.csv",timings)

def sequence(subject,arm,start,end,wind,n=31):
    # Both directions traverse the same arc. Keep the comparison's LOD budget
    # constant throughout both legs, including the stock grid/trunk budget.
    guardfile=OUT/"guard.json"
    limit=json.loads(guardfile.read_text()).get(arm,1) if guardfile.exists() else 1
    exe,args=command(subject,arm,start,132 if start==41 else 163,limit=limit,wind=wind)
    if "--spray-pixel-error" in args and subject=="crown":
        args[args.index("--spray-pixel-error")+1]=str(CROWN_BUDGET[100])
    args+=["--geometry-mask","--end-distance",str(end),"--end-pitch-degrees","-5.46" if end==41 else "-2.235",
           "--orbit-degrees",str(31 if start==41 else -31),"--frames",str(n),"--fps","15"]
    if wind:args+=["--animate-wind"]
    files=[exe,exe.parent/"assets"/TREE]
    if "--live-cut" in args:files.append(Path(args[args.index("--live-cut")+1]))
    key=hashlib.sha256(repr((args,[(p.stat().st_size,p.stat().st_mtime_ns) for p in files])).encode()).hexdigest()[:12]
    folder=OUT/"motion"/f"{subject}-{arm}-{start}-{wind}-{key}"
    folder.parent.mkdir(exist_ok=True)
    if not (folder/"frames.csv").exists():
        temp=folder.with_name(uuid.uuid4().hex)
        with folder.with_suffix(".log").open("w") as f:
            subprocess.run([exe,"--capture",temp,*args],check=True,stdout=f,stderr=f,timeout=240)
        temp.rename(folder)
    return folder

def motion():
    rows=[]
    for subject in ("crown","7","8"):
        for wind in (0,1):
            for start,end in ((41,100),(100,41)):
                arms=["source","current","sprays","native-compact","hybrid"]
                if (OUT/f"tiered-{subject}.oakcut").exists():arms.append("tiered")
                if subject=="crown" and (OUT/"canopy-crown.oakcut").exists():arms.append("canopy")
                if subject=="crown" and (OUT/"canopy1-crown.oakcut").exists():arms.append("canopy1")
                folders=[sequence(subject,a,start,end,wind) for a in arms]
                previous=None
                for frame in range(31):
                    masks=[read_mask(f/f"frame-{frame:06}.png") for f in folders]
                    ref=masks[0]
                    for j,q in enumerate(measure(ref,[(0,0,m) for m in masks[1:]]),1):
                        m=masks[j]
                        error=0 if previous is None else np.count_nonzero((m^previous[j])^(ref^previous[0]))
                        area=0 if previous is None else np.count_nonzero(m|ref|previous[j]|previous[0])
                        rows.append(dict(subject=subject,start=start,end=end,wind=wind,frame=frame,arm=arms[j],iou=q[2],
                                         edge=q[-1],temporal_fraction=error/max(1,area)))
                    previous=masks
                write_csv(OUT/"motion.csv",rows)
            print("motion",subject,wind,flush=True)

if __name__=="__main__":
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument("action",choices=["calibrate","freeze","verify","timing","suite","hybrid","guard","screen","motion","counters","order","canopy","canopy-check","canopy-time","frame-time","view","stage"])
    parser.add_argument("--subject",choices=["crown","7","8"],default="crown")
    parser.add_argument("--arm",choices=["current","production","sprays","native","native-compact","hybrid","tiered","canopy","canopy1","sprays-ordered","tiered-ordered","current-ordered"],default="sprays")
    parser.add_argument("--distance",type=int,choices=[41,100],default=100)
    parser.add_argument("--pixel-error",type=float)
    args=parser.parse_args()
    if args.action=="suite":
        freeze();verify();timing()
    elif args.action=="order":order_test()
    elif args.action=="frame-time":frame_time()
    elif args.action in ("canopy-check","canopy-time"):canopy(args.action=="canopy-check",args.action=="canopy-time")
    elif args.action in ("calibrate","freeze","verify","timing","hybrid","guard","screen","motion","counters","canopy"):globals()[args.action]()
    elif args.action in ("view","stage"):
        if args.subject!="crown" and args.arm=="production":parser.error("Production is the full tree")
        limits=settings() if chosen_path().exists() else {}
        base=args.arm.removesuffix("-ordered")
        if args.arm!=base and args.subject!="crown":parser.error("Static trunk packing uses the full crown")
        x=1 if base.startswith("native") else limits.get(f"{args.subject}/{args.distance}/{base}",1)
        guards=json.loads((OUT/"guard.json").read_text()) if (OUT/"guard.json").exists() else {"sprays":.9,"hybrid":.85}
        x=guards.get(base,x)
        if args.pixel_error is not None:x=args.pixel_error
        exe,flags=command(args.subject,args.arm,args.distance,132,x,probe_gi=None)
        if "--live-cut" in flags and not Path(flags[flags.index("--live-cut")+1]).is_file():
            raise FileNotFoundError(flags[flags.index("--live-cut")+1])
        if args.action=="view":subprocess.Popen([exe,"--review-capture",*flags])
        print(subprocess.list2cmdline([str(exe),"--review-capture",*flags]))
