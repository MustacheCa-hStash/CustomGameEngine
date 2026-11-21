"""Measure Unreal's moving four-oak triangle/voxel quality and cost curves."""
import argparse
import csv
import json
import subprocess
import tempfile
import time
from pathlib import Path
from statistics import median

import numpy as np
from PIL import Image, ImageDraw, ImageFont

from BenchmarkOakGpuTraversal import native_counter,launch_args
from CompareOakMaskEdges import outline, within_one_pixel
from MixedOakSpray import unreal
from OakGroveBricks import OUT as PREVIOUS, SEGMENTS, STEPS
from ReplayOakAssembly import BUILD, REPO, UE

OUT = BUILD / "oak-nanite-quality-sweep"
PIXELS = {"tri": (.85, .95, 1, 1.125, 1.25, 1.5, 1.75, 2, 2.25, 2.5, 3, 3.5),
          "vox": (.5, .65, .8, 1, 1.2, 1.4)}
FRAMES = 2 * STEPS
GUARD_CAP, GUARD_SCALE = 2.0, 140


def stem(start, arm, pixels):
    return f"oak-sweep-{start}-{arm}-{pixels:g}".replace(".", "p")


def job(start, arm, pixels, mode, name):
    asset = "Tree_Optimized_" + ("Triangles" if arm == "tri" else "Voxels")
    return dict(name=name, asset=asset, mesh_path="/Game/NaniteOakBench/"+asset, mode=mode,
                max_pixels_per_edge=pixels, steps=STEPS, distance=start,
                end_distance=dict(SEGMENTS)[start], trees=4, yaw=132, pitch=-2.235,
                frustum=1, hzb=0, resolution=[3840,2160], vertical_fov=45,
                isolated=True, capture_only=True)


def run(label, jobs):
    OUT.mkdir(exist_ok=True)
    manifest=OUT/(label+".json")
    manifest.write_text(json.dumps(jobs,indent=2))
    started=time.time_ns()
    unreal("CaptureMixedOakMotion.py",label,"OAK_CAPTURE CAPTURES_COMPLETE",
           dict(OAK_MIXED_DIR=str(OUT),OAK_GROVE_CAPTURE_JOBS=str(manifest),
                OAK_GROVE_MOTION_RIG=str(REPO/"Tools/CaptureOakGpuComparison.py")),OUT)
    for entry in jobs:
        name=entry["name"]
        files=([UE.parent/"Saved/Profiling/CSV"/(name+".csv")] if entry["mode"]=="Profile" else
               [OUT/"native"/f"{name}-{i:03}.png" for i in range(FRAMES)])
        if any(p.stat().st_mtime_ns<started for p in files):
            raise RuntimeError("Stale capture for "+name)


def sweep():
    jobs=[]
    for start,_ in SEGMENTS:
        settings=[(a,p) for a in PIXELS for p in PIXELS[a]]
        for arm,p in settings:
            jobs.append(job(start,arm,p,"Mask",stem(start,arm,p)))
        for repeat in range(2):
            for arm,p in (settings if repeat==0 else reversed(settings)):
                jobs.append(job(start,arm,p,"Profile",stem(start,arm,p)+f"-gpu-{repeat}"))
    run("sweep",jobs)


def csv_write(name, data):
    with (OUT/name).open("w",newline="") as f:
        writer=csv.DictWriter(f,data[0].keys())
        writer.writeheader()
        writer.writerows(data)


def read_rows(path):
    with path.open(newline="") as f:return list(csv.DictReader(f))


def mask(path):
    with Image.open(path) as image:
        if image.size!=(3840,2160):raise ValueError(path)
        rgb=np.asarray(image.convert("RGB"))
    return (rgb[:,:,1]>rgb[:,:,0]) & (rgb[:,:,1]>rgb[:,:,2])


def sequence_path(prefix, frame):
    return prefix.with_name(f"{prefix.name}-{frame:03}.png")


def load_set(prefix, roi):
    result=[]
    for i in range(FRAMES):
        full=mask(sequence_path(prefix,i))
        cropped=full[roi]
        if np.count_nonzero(full)!=np.count_nonzero(cropped) or not cropped.any():
            raise ValueError(f"Foreground outside search area in {prefix}, frame {i}")
        result.append(cropped.copy())
    return result


def foreground_roi(prefix):
    all_pixels=np.zeros((2160,3840),dtype=bool)
    for i in range(STEPS):
        all_pixels |= mask(sequence_path(prefix,i))
    ys,xs=np.where(all_pixels)
    return (slice(max(0,ys.min()-64),min(2160,ys.max()+65)),
            slice(max(0,xs.min()-64),min(3840,xs.max()+65)))


def score_masks(ref, target, candidate, start, label):
    result=[]
    for i,(fine,vox,test) in enumerate(zip(ref,target,candidate)):
        edge=outline(test)
        ref_edge=outline(fine)
        off=np.count_nonzero(ref_edge & ~within_one_pixel(edge))+np.count_nonzero(edge & ~within_one_pixel(ref_edge))
        temporal=None
        if i%STEPS:
            previous=candidate[i-1]
            original=ref[i-1]
            temporal=np.count_nonzero((test^previous)^(fine^original))/np.count_nonzero(test|previous|fine|original)
        k=min(i,FRAMES-1-i)
        result.append(dict(start=start,label=label,frame=i,distance=start+(dict(SEGMENTS)[start]-start)*k/(STEPS-1),
            iou=np.count_nonzero(test&fine)/np.count_nonzero(test|fine),
            outline=off/(np.count_nonzero(edge)+np.count_nonzero(ref_edge)),temporal=temporal,
            voxel_iou=np.count_nonzero(test&vox)/np.count_nonzero(test|vox)))
    return result


def gpu_samples(name):
    path=UE.parent/"Saved/Profiling/CSV"/(name+".csv")
    valid=[r for r in read_rows(path) if (r.get("FrameTime") or "").replace(".","",1).isdigit()]
    if len(valid)!=FRAMES+5:raise ValueError(f"Unexpected frame count in {path}")
    return [(float(r["GPU/NaniteVisBuffer"]),float(r["GPU/NaniteBasePass"]),float(r["GPUTime"]))
            for r in valid[3:3+FRAMES]]


def summarize(data, target):
    med=lambda k: median(r[k] for r in data if r[k] is not None)
    spatial=[r["iou"]>=v["iou"] and r["outline"]<=v["outline"] for r,v in zip(data,target)]
    motion=[r["temporal"]<=v["temporal"] for r,v in zip(data,target) if r["temporal"] is not None]
    return dict(iou=med("iou"),outline=med("outline"),temporal=med("temporal"),voxel_iou=med("voxel_iou"),
                spatial_pass=sum(spatial),motion_pass=sum(motion))


def reference(start):
    return PREVIOUS/"native"/f"grove-bricks-{start}-{dict(SEGMENTS)[start]}-fine"


def analyze_sweep(segments=SEGMENTS, profile=True, wait=False):
    frames=[]
    summary=[]
    for start,_ in segments:
        ref_path=reference(start)
        roi=foreground_roi(ref_path)
        ref=load_set(ref_path,roi)
        default_prefix=PREVIOUS/"native"/f"grove-bricks-{start}-{dict(SEGMENTS)[start]}-vox"
        target=load_set(default_prefix,roi)
        baseline=score_masks(ref,target,target,start,"vox-1")
        for arm,pixels in PIXELS.items():
            for p in pixels:
                name=stem(start,arm,p)
                if wait:
                    deadline=time.monotonic()+1200
                    while not sequence_path(OUT/"native"/name,FRAMES-1).is_file():
                        if time.monotonic()>deadline:raise TimeoutError(name)
                        time.sleep(2)
                    time.sleep(.3)
                data=score_masks(ref,target,load_set(OUT/"native"/name,roi),start,f"{arm}-{p:g}")
                if arm=="vox" and p==1 and any(r["voxel_iou"]!=1 for r in data):
                    raise ValueError("Current capture does not register with the previous default mask")
                frames+=data
                row=dict(start=start,arm=arm,pixels=p,**summarize(data,baseline))
                if profile:
                    timings=[median(v+b for v,b,_ in gpu_samples(name+f"-gpu-{rep}")) for rep in range(2)]
                    row.update(tree_ms=median(timings),run0=timings[0],run1=timings[1])
                summary.append(row)
            csv_write(f"quality-{start}-{arm}.csv",[r for r in frames if r["start"]==start and r["label"].startswith(arm+"-")])
        print(f"Scored {start} m clip",flush=True)
    csv_write("curves.csv",summary)
    csv_write("sweep-frames.csv",frames)


def plan():
    curves=read_rows(OUT/"curves.csv")
    samples=read_rows(OUT/"sweep-frames.csv")
    for row in curves:
        name=stem(int(row["start"]),row["arm"],float(row["pixels"]))
        repeats=[median(v+b for v,b,_ in gpu_samples(name+f"-gpu-{rep}")) for rep in range(2)]
        row.update(tree_ms=median(repeats),run0=repeats[0],run1=repeats[1])
    csv_write("curves.csv",curves)
    choices={}
    training=[]
    errors=("iou","outline","temporal")
    values=np.asarray(PIXELS["tri"])
    for start,_ in SEGMENTS:
        clip=[r for r in curves if int(r["start"])==start]
        target=next(r for r in clip if r["arm"]=="vox" and float(r["pixels"])==1)
        tris=[r for r in clip if r["arm"]=="tri"]
        nearest=min(tris,key=lambda r:max(abs(float(r[k])-float(target[k])) for k in errors))
        qualified=[r for r in tris if int(r["spatial_pass"])==FRAMES and int(r["motion_pass"])==FRAMES-2]
        safe=max(qualified,key=lambda r:float(r["pixels"]))
        choices[str(start)]=dict(closest=float(nearest["pixels"]),fixed=float(safe["pixels"]))
        clip=[r for r in samples if int(r["start"])==start]
        for k in range(0,STEPS,2):
            at={r["label"]:r for r in clip if int(r["frame"])==k}
            target=at["vox-1"]
            dist=float(target["distance"])
            for key in errors:
                if not target[key]:continue
                errors_at_p=[float(at[f"tri-{p:g}"][key]) for p in values]
                threshold=float(target[key])
                if key=="iou":
                    errors_at_p=[1-x for x in errors_at_p]
                    threshold=1-threshold
                training.append((dist,errors_at_p,threshold))
    # Two parameters fit the outward even poses. Measure the actual changing cuts on both
    # legs and a second yaw. The margin accounts for interpolated rather than sampled cuts.
    feasible=[]
    for cap in np.arange(1.6,2.81,.05):
        for scale in range(115,181):
            def budget(d):return min(cap,scale/d)
            if all(np.interp(budget(d),values,err)<=.94*target for d,err,target in training):
                feasible.append((sum(np.log(budget(d)) for d,_,_ in training),cap,scale))
    if not feasible:raise ValueError("No training schedule passed with headroom")
    _,cap,scale=max(feasible)
    choices["schedule"]=dict(cap=round(cap,2),scale=scale)
    (OUT/"choices.json").write_text(json.dumps(choices,indent=2))
    return choices


def confirm():
    choices=json.loads((OUT/"choices.json").read_text())
    jobs=[]
    for start,end in SEGMENTS:
        chosen=choices[str(start)]
        cap,scale=(choices["schedule"][k] for k in ("cap","scale"))
        schedule=[round(min(cap,scale/(start+(end-start)*k/(STEPS-1))),4) for k in range(STEPS)]
        settings=dict(vox=("vox",1),fixed=("tri",chosen["fixed"]),closest=("tri",chosen["closest"]),
                      adaptive=("tri",schedule[0]),fine=("tri",.001))
        def add(label,mode,tag,yaw=132):
            arm,p=settings[label]
            entry=job(start,arm,p,mode,f"oak-pair-{start}-{yaw}-{label}-{tag}")
            entry["yaw"]=yaw
            if label=="adaptive":entry["pixel_schedule"]=schedule
            jobs.append(entry)
        add("adaptive","Mask","mask")
        for label in settings:
            add(label,"Lit","lit")
            if label!="fine":add(label,"OffscreenStats","count")
        for label in ("fine","vox","adaptive"):
            add(label,"Mask","mask",23)
        # Balance ordering across repeated profile blocks. Image export and diagnostics are off.
        order=["vox","closest","fixed","adaptive"]
        for rep in range(6):
            current=order[rep%4:]+order[:rep%4]
            if rep%2:current=current[::-1]
            for label in current:add(label,"Profile",f"gpu-{rep}")
        # A second pair tests overlapping error curves independently of the default voxel cut.
        a,b=(1.5,.65) if start==50 else (1.25,.5)
        for rep in range(6):
            pair=(("tri",a),("vox",b))
            for arm,p in (pair if rep%2==0 else reversed(pair)):
                jobs.append(job(start,arm,p,"Profile",f"oak-overlap-{start}-{arm}-{rep}"))
    run("confirm",jobs)


def custom(guard=False):
    choices=json.loads((OUT/"choices.json").read_text())
    cap,scale=(choices["schedule"][k] for k in ("cap","scale"))
    for start,end in SEGMENTS:
        for repeat in range(-1,4):
            labels=("guard",) if guard else (("adaptive","fixed") if repeat%2==0 else ("fixed","adaptive"))
            for label in labels:
                for leg,(a,b) in enumerate(((start,end),(end,start))):
                    budget=GUARD_CAP if guard else cap if label=="adaptive" else choices[str(start)]["fixed"]
                    exe,args=launch_args("native-gpu",4,a,132,budget,probe_gi=None)
                    args[args.index("--pitch-degrees")+1]="-2.235"
                    if label in ("adaptive","guard"):
                        args+=["--live-cut-error-distance-product",str(GUARD_SCALE if guard else scale)]
                    flags=["--geometry-mask","--probe-gi","off"] if repeat<0 else ["--sequence-timings-only"]
                    dest=OUT/f"custom-{start}-{label}-{repeat}-{leg}"
                    dest.mkdir(exist_ok=True)
                    with tempfile.TemporaryDirectory(dir=OUT) as tmp:
                        path=Path(tmp)/"frames"
                        with (OUT/"custom.log").open("a") as log:
                            subprocess.run([exe,"--capture",path,*args,"--frames",str(STEPS),"--end-distance",str(b),
                                            "--fps","30",*flags],stdout=log,stderr=log,check=True,timeout=300)
                        if len(read_rows(path/"frames.csv"))!=STEPS:raise ValueError(dest)
                        for file in path.iterdir():file.replace(dest/file.name)


def confirmation_report():
    choices=json.loads((OUT/"choices.json").read_text())
    result=[]
    run_times=[]
    color=[]
    evidence=[]
    for start,_ in SEGMENTS:
        for yaw in (132,23):
            base=OUT/"native"/f"oak-pair-{start}-{yaw}"
            def pref(label,tag):return base.with_name(base.name+f"-{label}-{tag}")
            fine=reference(start) if yaw==132 else pref("fine","mask")
            vox=OUT/"native"/stem(start,"vox",1) if yaw==132 else pref("vox","mask")
            roi=foreground_roi(fine)
            ref=load_set(fine,roi)
            target=load_set(vox,roi)
            target_scores=score_masks(ref,target,target,start,"vox")
            labels=["vox","adaptive","fixed","closest"] if yaw==132 else ["vox","adaptive"]
            for label in labels:
                if label=="vox":candidate=target
                elif label=="adaptive":candidate=load_set(pref(label,"mask"),roi)
                else:candidate=load_set(OUT/"native"/stem(start,"tri",choices[str(start)][label]),roi)
                data=score_masks(ref,target,candidate,start,label)
                record=dict(start=start,yaw=yaw,label=label,**summarize(data,target_scores))
                evidence.extend(dict(yaw=yaw,**r) for r in data)
                record.update(triangles=None,bricks=None,tree_ms=None,vis_ms=None,base_ms=None,gpu_ms=None)
                if yaw==132:
                    tris=[];bricks=[]
                    for f in range(FRAMES):
                        path=sequence_path(pref(label,"count"),f)
                        tris.append(native_counter(path,424))
                        bricks.append(native_counter(path,456))
                    runs=[]
                    for rep in range(6):
                        tim=gpu_samples(pref(label,f"gpu-{rep}").name)
                        run=dict(start=start,rep=rep,label=label,tree=median(v+b for v,b,_ in tim),
                                 vis=median(v for v,_,_ in tim),base=median(b for _,b,_ in tim),gpu=median(t for _,_,t in tim))
                        runs.append(run)
                    run_times.extend(runs)
                    record.update(triangles=median(tris),bricks=median(bricks),tree_ms=median(r["tree"] for r in runs),
                        vis_ms=median(r["vis"] for r in runs),base_ms=median(r["base"] for r in runs),gpu_ms=median(r["gpu"] for r in runs))
                    if label!="vox":
                        def rgb(which,f):
                            with Image.open(sequence_path(pref(which,"lit"),f)) as image:
                                return np.array(image.crop((roi[1].start,roi[0].start,roi[1].stop,roi[0].stop)).convert("RGB"),dtype=np.int16)
                        for f in range(FRAMES):
                            tri_lit=rgb(label,f)
                            vox_lit=rgb("vox",f)
                            fine_lit=rgb("fine",f)
                            union=ref[f]|target[f]|candidate[f]
                            delta=np.abs(tri_lit[union]-vox_lit[union])
                            color.append(dict(start=start,frame=f,label=label,mean_rgb_difference=float(delta.mean()),
                                rgb_changed_fraction=float((delta.max(axis=1)>0).mean()),
                                rgb_difference_over_8=float((delta.max(axis=1)>8).mean()),
                                tri_to_fine=float(np.abs(tri_lit[union]-fine_lit[union]).mean()),
                                vox_to_fine=float(np.abs(vox_lit[union]-fine_lit[union]).mean())))
                result.append(record)
    csv_write("confirmed.csv",result)
    csv_write("confirmed-frames.csv",evidence)
    csv_write("paired-runs.csv",run_times)
    csv_write("shaded.csv",color)


def custom_report(guard=False):
    results=[]
    for start,_ in SEGMENTS:
        fine=reference(start)
        roi=foreground_roi(fine)
        ref=load_set(fine,roi)
        target=load_set(OUT/"native"/stem(start,"vox",1),roi)
        baseline=score_masks(ref,target,target,start,"vox")
        for label in (("guard",) if guard else ("adaptive","fixed")):
            masks=[];counts=[]
            runs=[]
            for rep in range(-1,4):
                times=[]
                for leg in (0,1):
                    folder=OUT/f"custom-{start}-{label}-{rep}-{leg}"
                    series=read_rows(folder/"frames.csv")
                    if rep<0:
                        for f,r in enumerate(series):
                            with Image.open(folder/f"frame-{f:06}.png") as image:
                                full=np.asarray(image.convert("L"))!=0
                                if full[roi].sum()!=full.sum():raise ValueError("D3D mask exceeds crop")
                                masks.append(full[roi].copy())
                            counts.append(sum(int(r[k]) for k in ("camera_leaf_triangles","camera_spray_wood_triangles","camera_tree_trunk_triangles")))
                    else:times.extend(float(r["gpu_asset_ms"]) for r in series)
                if times:runs.append(median(times))
            scores=score_masks(ref,target,masks,start,label)
            results.append(dict(start=start,label=label,triangles=median(counts),tree_ms=median(runs),
                runs=runs,**summarize(scores,baseline)))
    csv_write("custom-guard.csv" if guard else "custom.csv",results)


def illustration():
    choices=json.loads((OUT/"choices.json").read_text())
    width,height=244,154
    sheet=Image.new("RGB",(4*width+40,2*(height+42)+45),(248,248,245))
    draw=ImageDraw.Draw(sheet)
    font=ImageFont.truetype("segoeui.ttf",16)
    for col,name in enumerate(("Fine triangle reference","Tuned triangles","Default voxels","Occupancy differences")):
        draw.text((8+(width+8)*col,5),name,font=font,fill="black")
    for row,(start,end) in enumerate(SEGMENTS):
        f=STEPS-1
        fine=mask(sequence_path(reference(start),f))
        edges=outline(fine)
        ys,xs=np.where(fine)
        _,x,y=max((edges[y:y+height,x:x+width].sum(),x,y)
                  for y in range(max(0,ys.min()-height//3),ys.max(),16)
                  for x in range(max(0,xs.min()-width//3),xs.max(),16)
                  if x+width<=fine.shape[1] and y+height<=fine.shape[0])
        upper=32+row*(height+42)
        for col,label in enumerate(("fine","closest","vox")):
            name=f"oak-pair-{start}-132-{label}-lit"
            with Image.open(sequence_path(OUT/"native"/name,f)) as im:
                sheet.paste(im.crop((x,y,x+width,y+height)).convert("RGB"),(8+(width+8)*col,upper))
        t=mask(sequence_path(OUT/"native"/stem(start,"tri",choices[str(start)]["closest"]),f))[y:y+height,x:x+width]
        v=mask(sequence_path(OUT/"native"/stem(start,"vox",1),f))[y:y+height,x:x+width]
        diff=np.zeros((height,width,3),dtype=np.uint8)
        diff[t&v]=(85,85,85)
        diff[t&~v]=(0,200,240)
        diff[~t&v]=(245,90,160)
        sheet.paste(Image.fromarray(diff),(8+(width+8)*3,upper))
        draw.text((8,upper+height+2),f"{end} m     1 image pixel per render pixel",font=font,fill="black")
    draw.text((8,sheet.height-24),"Cyan shows triangle-only pixels. Pink shows voxel-only pixels. Gray shows shared coverage.",font=font,fill="black")
    sheet.save(OUT/"grove-crops.png")


def overlap_report():
    curves=read_rows(OUT/"curves.csv")
    results=[]
    for start,_ in SEGMENTS:
        clip=[r for r in curves if int(r["start"])==start]
        for v in [r for r in clip if r["arm"]=="vox"]:
            def gap(t):return max(abs(float(t[k])-float(v[k])) for k in ("iou","outline","temporal"))
            t=min((r for r in clip if r["arm"]=="tri"),key=gap)
            results.append(dict(start=start,vox_pixels=v["pixels"],tri_pixels=t["pixels"],max_error_gap=gap(t),
                                vox_ms=v["tree_ms"],tri_ms=t["tree_ms"]))
    csv_write("overlaps.csv",results)
    paired=[]
    for start,_ in SEGMENTS:
        for rep in range(6):
            entry=dict(start=start,repeat=rep)
            for arm in ("tri","vox"):
                entry[arm]=median(v+b for v,b,_ in gpu_samples(f"oak-overlap-{start}-{arm}-{rep}"))
            paired.append(entry)
    csv_write("overlap-runs.csv",paired)


def guard_test():
    jobs=[]
    for start,end in SEGMENTS:
        schedule=[round(min(GUARD_CAP,GUARD_SCALE/(start+(end-start)*k/(STEPS-1))),4) for k in range(STEPS)]
        def add(yaw,label,mode,tag):
            p={"guard":schedule[0],"vox":1,"fine":.001}[label]
            entry=job(start,"vox" if label=="vox" else "tri",p,mode,f"oak-guard-{start}-{yaw}-{label}-{tag}")
            entry["yaw"]=yaw
            if label=="guard":entry["pixel_schedule"]=schedule
            jobs.append(entry)
        for yaw in (132,23,197):
            add(yaw,"guard","Mask","mask")
            if yaw==197:
                add(yaw,"fine","Mask","mask")
                add(yaw,"vox","Mask","mask")
        add(132,"guard","OffscreenStats","count")
        add(132,"vox","Lit","repeat-lit")
        for arm,p in (("tri",1.5 if start==50 else 1.25),("vox",.65 if start==50 else .5)):
            jobs.append(job(start,arm,p,"OffscreenStats",f"oak-overlap-{start}-{arm}-count"))
        for rep in range(6):
            for label in (("guard","vox") if rep%2==0 else ("vox","guard")):
                add(132,label,"Profile",f"gpu-{rep}")
    run("guard",jobs)


def guard_report():
    data=[]
    repeats=[]
    counts=[]
    noise=[]
    for start,_ in SEGMENTS:
        for yaw in (132,23,197):
            prefix=OUT/"native"/f"oak-guard-{start}-{yaw}"
            def frame_set(label):return prefix.with_name(prefix.name+f"-{label}-mask")
            if yaw==132:
                fine=reference(start)
                vox=OUT/"native"/stem(start,"vox",1)
            elif yaw==23:
                fine=OUT/"native"/f"oak-pair-{start}-23-fine-mask"
                vox=OUT/"native"/f"oak-pair-{start}-23-vox-mask"
            else:
                fine,vox=frame_set("fine"),frame_set("vox")
            roi=foreground_roi(fine)
            ref,target=load_set(fine,roi),load_set(vox,roi)
            baseline=score_masks(ref,target,target,start,"vox")
            records=score_masks(ref,target,load_set(frame_set("guard"),roi),start,"guard")
            data.append(dict(start=start,yaw=yaw,**summarize(records,baseline),
                worst_outline_excess=max(r["outline"]-t["outline"] for r,t in zip(records,baseline)),
                worst_iou_deficit=max(t["iou"]-r["iou"] for r,t in zip(records,baseline))))
            if yaw==132:
                box=(roi[1].start,roi[0].start,roi[1].stop,roi[0].stop)
                for i in range(FRAMES):
                    def rgb(prefix):
                        with Image.open(sequence_path(OUT/"native"/prefix,i)) as im:
                            return np.array(im.crop(box).convert("RGB"),dtype=np.int16)
                    delta=rgb(f"oak-pair-{start}-132-vox-lit")-rgb(f"oak-guard-{start}-132-vox-repeat-lit")
                    noise.append(dict(start=start,frame=i,repeat_rgb_mae=np.abs(delta[target[i]]).mean()))
        for label,stem_name in (("guard",f"oak-guard-{start}-132-guard-count"),
                                ("overlap-tri",f"oak-overlap-{start}-tri-count"),
                                ("overlap-vox",f"oak-overlap-{start}-vox-count")):
            for i in range(FRAMES):
                file=sequence_path(OUT/"native"/stem_name,i)
                counts.append(dict(start=start,label=label,frame=i,triangles=native_counter(file,424),
                                   bricks=native_counter(file,456)))
        for rep in range(6):
            row=dict(start=start,repeat=rep)
            for arm in ("guard","vox"):
                row[arm]=median(v+b for v,b,_ in gpu_samples(f"oak-guard-{start}-132-{arm}-gpu-{rep}"))
            repeats.append(row)
    csv_write("guard-quality.csv",data)
    csv_write("guard-gpu.csv",repeats)
    csv_write("guard-counts.csv",counts)
    csv_write("repeat-color.csv",noise)


def color_control():
    choices=json.loads((OUT/"choices.json").read_text())
    data=[]
    for start,_ in SEGMENTS:
        roi=foreground_roi(reference(start))
        ys,xs=roi
        box=(xs.start//4*4,ys.start//4*4,min(3840,(xs.stop+3)//4*4),min(2160,(ys.stop+3)//4*4))
        for i in range(FRAMES):
            def crop(prefix):
                with Image.open(sequence_path(prefix,i)) as im:return np.asarray(im.crop(box).convert("RGB"))
            def coverage(prefix):
                rgb=crop(prefix)
                return (rgb[:,:,1]>rgb[:,:,0])&(rgb[:,:,1]>rgb[:,:,2])
            folder=OUT/"native"
            union=coverage(reference(start)) | coverage(folder/stem(start,"vox",1)) | coverage(folder/stem(start,"tri",choices[str(start)]["closest"]))
            voxel=crop(folder/f"oak-pair-{start}-132-vox-lit").astype(np.int16)
            h,w=union.shape
            blocks=union.reshape(h//4,4,w//4,4).any(axis=(1,3))
            row=dict(start=start,frame=i)
            for label,prefix in (("tri",f"oak-pair-{start}-132-closest-lit"),("repeat",f"oak-guard-{start}-132-vox-repeat-lit")):
                delta=crop(folder/prefix)-voxel
                row[label+"_pixel"]=np.abs(delta[union]).mean()
                row[label+"_block"]=np.abs(delta.reshape(h//4,4,w//4,4,3).mean(axis=(1,3))[blocks]).mean()
            data.append(row)
    csv_write("color-control.csv",data)


if __name__=="__main__":
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument("action",choices=("sweep","score","plan","confirm","report","custom","custom-report","illustrate","overlap","guard","guard-report","custom-guard","color-control"),default="sweep",nargs="?")
    args=parser.parse_args()
    if args.action=="sweep":sweep()
    elif args.action=="score":analyze_sweep()
    elif args.action=="plan":plan()
    elif args.action=="confirm":confirm()
    elif args.action=="report":confirmation_report()
    elif args.action=="custom":custom()
    elif args.action=="custom-report":custom_report()
    elif args.action=="illustrate":illustration()
    elif args.action=="guard":guard_test()
    elif args.action=="guard-report":guard_report()
    elif args.action=="color-control":color_control()
    elif args.action=="custom-guard":
        custom(guard=True)
        custom_report(guard=True)
    else:overlap_report()
