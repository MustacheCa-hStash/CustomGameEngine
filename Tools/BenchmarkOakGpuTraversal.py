"""Compare submitted camera geometry, two silhouette gates and shaded frame times on fixed oak layouts."""
import argparse
import csv
import hashlib
import json
import subprocess
import tempfile
from pathlib import Path
from statistics import median
from functools import lru_cache
from PIL import Image, ImageDraw, ImageFont

import numpy as np

from BenchmarkOakLiveCuts import command, settings
from CompareOakMaskEdges import measure, read_mask
from ReplayOakAssembly import BUILD, UE
from DecodeOakBoughVoxels import write_csv

OUT = BUILD / "oak-gpu-traversal"
ANGLES = (132, 23, 197)
# Seven raster rows for each decimal character in UE's ShaderPrint counter font.
DIGITS = ((30,51,55,63,59,51,30), (12,60,12,12,12,12,63), (30,51,3,14,24,51,63),
          (30,51,3,14,3,51,30), (14,30,54,102,127,6,6), (63,48,62,3,3,51,30),
          (14,24,48,62,51,51,30), (63,51,3,6,12,24,24), (30,51,51,30,51,51,30),
          (30,51,51,31,3,6,28))


def native_counter(path, y):
    with Image.open(path) as image:
        assert image.size == (3840,2160)
        ink=(np.asarray(image.crop((3065,y,3175,y+7)).convert("RGB"))==255).all(axis=2)
    digits=[]
    x=0
    while x<ink.shape[1]:
        if not ink[:,x].any():
            x+=1
            continue
        for d,rows in enumerate(DIGITS):
            width=max(rows).bit_length()
            if x+width>ink.shape[1]: continue
            bits=ink[:,x:x+width] @ (1 << np.arange(width-1,-1,-1))
            if np.array_equal(bits,rows):
                digits.append(str(d))
                x+=width
                break
        else:
            raise ValueError(f"Unrecognized native counter at {path}, x={3065+x}, y={y}")
    return int("".join(digits))
FAMILIES = ("production", "tiered", "canopy1", "native")


def launch_args(arm, trees, distance, yaw, error, probe_gi="off"):
    family = arm.split("-")[0]
    path = "native-compact" if arm=="native-cpu" else family
    exe, args = command("crown", path, distance, yaw, error, probe_gi=probe_gi)
    if arm.endswith(("-gpu","-packed")):
        if "--live-cut-pack-assembly" in args:
            args.remove("--live-cut-pack-assembly")
        args.append("--live-cut-gpu")
        if arm.endswith("-packed"): args.append("--live-cut-pack-assembly")
    args[args.index("--pitch-degrees")+1] = "-2.235" if distance==100 else "-5.46"
    args += ["--tree-count", str(trees)]
    return exe, args


def capture(arm, trees, distance, yaw, error, repeat=None, counters=False):
    exe, args = launch_args(arm, trees, distance, yaw, error)
    args += ["--timing-samples", "3" if repeat is None else "61"]
    if counters: args.append("--camera-pipeline-stats")
    if repeat is None:
        args += ["--geometry-mask"]
    assets = [exe, exe.parent/"assets/WhiteOak-ForkedCanopy.treelod"]
    if "--live-cut" in args:
        assets.append(Path(args[args.index("--live-cut")+1]))
    key = repr((args,repeat,[p.stat().st_mtime_ns for p in assets]))
    path = OUT/"captures"/f"{arm}-{trees}-{distance}-{yaw}-{hashlib.sha256(key.encode()).hexdigest()[:12]}.png"
    path.parent.mkdir(exist_ok=True, parents=True)
    timings=path.with_suffix(".timings.csv")
    if not path.exists() or not timings.exists():
        with tempfile.TemporaryDirectory(dir=path.parent) as folder, path.with_suffix(".log").open("w") as log:
            temp=Path(folder)/"frame.png"
            subprocess.run([exe, "--capture", temp, *args], check=True, stdout=log, stderr=log, timeout=180)
            temp.with_suffix(".timings.csv").replace(timings)
            temp.replace(path)
    with timings.open() as f:
        rows=list(csv.DictReader(f))
    fields=("gpu_asset_ms", "gpu_live_cut_ms", "gpu_camera_ms", "gpu_frame_ms", "fenced_frame_ms", "camera_tree_draws",
            "camera_tree_submission_calls","camera_gpu_tree_dispatches","camera_leaf_triangles",
            "camera_spray_wood_triangles","camera_tree_trunk_triangles","shadow_triangles")
    result={k:median(float(r[k]) for r in rows) for k in fields}
    triangle_fields=("camera_leaf_triangles","camera_spray_wood_triangles","camera_tree_trunk_triangles")
    assert len({tuple(r[k] for k in triangle_fields) for r in rows})==1
    result["tris"]=int(sum(result[k] for k in triangle_fields))
    if counters:
        for k in ("camera_mesh_vs_invocations","camera_mesh_ps_invocations"):
            result[k]=int(rows[0][k])
    return path, result


def score(reference, candidate):
    row=next(measure(reference, [(0,0,candidate)]))
    return row[2], row[-1]


@lru_cache(3)
def references(n,d,y):
    root=UE/f"gpu-oak-{n}-{d}-{y}"
    native=read_mask(Path(str(root)+"-1.png"), True)
    fine=read_mask(Path(str(root)+"-0.001.png"), True)
    own=read_mask(capture("source",n,d,y,1)[0])
    return native,fine,own,score(fine,native)


def quality(arm,n,d,y,x):
    native,fine,own,(gi,ge)=references(n,d,y)
    image,data=capture(arm,n,d,y,x)
    mask=read_mask(image)
    ci,ce=score(fine,mask)
    oi,oe=score(own,mask)
    return dict(trees=n,d=d,yaw=y,arm=arm,error=x,tris=data["tris"],common_iou=ci,common_edge=ce,
                own_iou=oi,own_edge=oe,gate_iou=gi,gate_edge=ge,passed=ci>=gi and ce<=ge and oi>=gi and oe<=ge,
                fine_alignment=score(fine,own)[0], native_agreement=score(native,mask)[0])


def qualify():
    rows=[]; frozen={}; parity=[]
    for n in (1,4):
        for d in (24,41,100):
            for family in FAMILIES:
                arm=family if family=="production" else family+"-gpu"
                seed=1.
                failed=None
                previous=settings().get(f"crown/{d}/{family}",1.)
                for x in sorted({seed, previous,.97,.94,.85,.70,.5,.35,.18},reverse=True):
                    if x>seed: continue
                    row=quality(arm,n,d,132,x)
                    rows.append(row)
                    if row["passed"]: break
                    failed=x
                if not row["passed"]:
                    raise RuntimeError(f"No matched primary silhouette for {arm} at {n}/{d}")
                # Refine the primary camera before freezing the same setting for both holdouts.
                if failed is not None:
                    for _ in range(4):
                        mid=round((x+failed)*.5,6)
                        trial=quality(arm,n,d,132,mid)
                        rows.append(trial)
                        if trial["passed"]: x=mid
                        else: failed=mid
                rows.append(quality(arm,n,d,132,x))
                frozen[f"{n}/{d}/{family}"]=x
                for yaw in ANGLES[1:]:
                    rows.append(quality(arm,n,d,yaw,x))
                if family!="production":
                    for yaw in ANGLES:
                        gpu,g=capture(arm,n,d,yaw,x)
                        cpu,c=capture(family+"-cpu",n,d,yaw,x)
                        delta=np.count_nonzero(read_mask(gpu)!=read_mask(cpu))
                        parity.append(dict(trees=n,d=d,yaw=yaw,arm=family,error=x,gpu_tris=g["tris"],
                                           cpu_tris=c["tris"],mask_pixels_changed=int(delta)))
                    write_csv(OUT/"parity.csv",parity)
                    if any(p["gpu_tris"]!=p["cpu_tris"] or p["mask_pixels_changed"] for p in parity[-3:]):
                        print("CPU/GPU differences",parity[-3:],flush=True)
                print("quality",n,d,arm,x,[r["passed"] for r in rows[-3:]],flush=True)
                write_csv(OUT/"quality.csv",rows)
                (OUT/"settings.json").write_text(json.dumps(frozen,indent=2))


def frozen_settings():
    frozen=json.loads(Path(__file__).with_name("OakGpuTraversalSettings.json").read_text())
    path=OUT/"settings.json"
    if path.exists(): frozen.update(json.loads(path.read_text()))
    return frozen


def verify_frozen():
    errors=frozen_settings()
    quality_rows=[]; parity=[]; nominal=[]
    for n in (1,4):
        for d in (24,41,100):
            nominal.append(quality("native-gpu",n,d,132,1.))
            for family in FAMILIES:
                x=errors[f"{n}/{d}/{family}"]
                arm=family if family=="production" else family+"-gpu"
                for yaw in ANGLES:
                    row=quality(arm,n,d,yaw,x)
                    quality_rows.append(row)
                    if yaw==132 and not row["passed"]: raise ValueError(row)
                    if family=="production": continue
                    cpu,c=capture(family+"-cpu",n,d,yaw,x)
                    control=read_mask(cpu)
                    candidates=[arm]+([family+"-packed"] if family in ("native","canopy1") else [])
                    for candidate in candidates:
                        path,g=capture(candidate,n,d,yaw,x)
                        changed=int(np.count_nonzero(control!=read_mask(path)))
                        parity.append(dict(trees=n,d=d,yaw=yaw,arm=candidate,error=x,gpu_tris=g["tris"],
                                           cpu_tris=c["tris"],mask_pixels_changed=changed))
                        if changed or c["tris"]!=g["tris"]: raise ValueError(parity[-1])
            write_csv(OUT/"qualified.csv",quality_rows)
            write_csv(OUT/"encoder-parity.csv",parity)
            write_csv(OUT/"nominal.csv",nominal)
            print("verified all encoders",n,d,flush=True)


def motion():
    # Exercise changing bins and packed index counts while reusing the same GPU resources.
    rows=[]
    for n in (1,4):
        for arm in ("native-cpu","native-packed","canopy1-cpu","canopy1-gpu"):
            exe,args=launch_args(arm,n,24,132,1.)
            args[args.index("--wind-strength")+1]=".65"
            args+=["--frames","7","--end-distance","100","--end-pitch-degrees","-2.235","--orbit-degrees","65",
                   "--animate-wind","--geometry-mask"]
            key=hashlib.sha256(repr((args,exe.stat().st_mtime_ns)).encode()).hexdigest()[:12]
            folder=OUT/f"motion-{arm}-{n}-{key}"
            if not (folder/"frames.csv").exists():
                with tempfile.TemporaryDirectory(dir=OUT) as scratch:
                    sequence=Path(scratch)/"sequence"
                    subprocess.run([exe,"--capture",sequence,*args],check=True,timeout=180)
                    sequence.replace(folder)
            with (folder/"frames.csv").open() as stream: manifest=list(csv.DictReader(stream))
            if arm.endswith("-cpu"):
                control,counts=folder,manifest
                continue
            for i,r in enumerate(manifest):
                name=f"frame-{i:06}.png"
                delta=int(np.count_nonzero(read_mask(control/name)!=read_mask(folder/name)))
                rows.append(dict(trees=n,arm=arm,frame=i,distance=r["distance"],triangles=r["camera_triangles"],
                                 cpu_triangles=counts[i]["camera_triangles"],mask_pixels_changed=delta))
                if delta or counts[i]["camera_triangles"]!=r["camera_triangles"]: print("motion delta",rows[-1],flush=True)
    write_csv(OUT/"motion-parity.csv",rows)


def timings():
    errors=frozen_settings()
    rows=[]
    for n in (1,4):
        for d in (24,41,100):
            arms=["production", "tiered-cpu","tiered-gpu","canopy1-cpu","canopy1-gpu","canopy1-packed","native-cpu","native-gpu","native-packed"]
            for rep in range(3):
                for arm in (arms if rep%2==0 else list(reversed(arms))):
                    x=errors[f"{n}/{d}/{arm.split('-')[0]}"]
                    _,r=capture(arm,n,d,132,x,rep)
                    rows.append(dict(trees=n,d=d,arm=arm,repeat=rep,error=x,**r))
                write_csv(OUT/"timings.csv",rows)
                print("timing",n,d,rep,flush=True)


def native_profiles():
    rows=[]
    for n in (1,4):
        for d in (24,41,100):
            for repeat in range(2):
                path=UE.parent/"Saved/Profiling/CSV"/f"gpu-oak-{n}-{d}-profile-{repeat}.csv"
                with path.open() as f:
                    records=[r for r in csv.DictReader(f) if (r.get("FrameTime") or "").replace(".","",1).isdigit()][8:-5]
                assert len(records)>=40
                rows.append(dict(trees=n,d=d,repeat=repeat,tris=native_counter(UE/f"gpu-oak-{n}-{d}-stats.png",424),samples=len(records),
                    gpu_camera_ms=median(float(r["GPU/NaniteVisBuffer"])+float(r["GPU/NaniteBasePass"]) for r in records),
                    gpu_frame_ms=median(float(r["GPUTime"]) for r in records),
                    frame_ms=median(float(r["FrameTime"]) for r in records)))
    write_csv(OUT/"native-profiles.csv",rows)
    rows=[]; counters=[]
    for n in (1,4):
        for d in (24,41,100):
            stats=OUT/"game"/f"fenced-{n}-{d}-0-stats.png"
            tris=native_counter(stats,424)
            assert tris==native_counter(UE/f"gpu-oak-{n}-{d}-stats.png",424)
            bricks=native_counter(stats,456)
            sw,hw,clusters=(native_counter(stats,y) for y in (200,216,408))
            assert bricks==0 and sw+hw==clusters
            counters.append(dict(trees=n,d=d,triangles=tris,bricks=bricks,software_clusters=sw,hardware_clusters=hw))
            for mode in ("fenced","pipelined"):
                for repeat in range(2):
                    stem=OUT/"game"/f"{mode}-{n}-{d}-{repeat}"
                    with stem.with_suffix(".csv").open() as f:
                        records=[r for r in csv.DictReader(f) if (r.get("FrameTime") or "").replace(".","",1).isdigit()][8:-5]
                    assert len(records)>=40
                    active=None
                    if mode=="fenced":
                        with stem.with_name(stem.name+"-wall.csv").open() as f:
                            active=median(float(r["active_frame_ms"]) for r in list(csv.DictReader(f))[8:-5])
                    def mid(k): return median(float(r[k]) for r in records)
                    rows.append(dict(trees=n,d=d,repeat=repeat,mode=mode,tris=tris,samples=len(records),
                        gpu_camera_ms=median(float(r["GPU/NaniteVisBuffer"])+float(r["GPU/NaniteBasePass"]) for r in records),
                        gpu_shadow_ms=mid("GPU/ShadowDepths"),gpu_frame_ms=mid("GPUTime"),frame_ms=mid("FrameTime"),
                        fenced_frame_ms=active))
    write_csv(OUT/"game-profiles.csv",rows)
    write_csv(OUT/"native-counters.csv",counters)


def check_guards():
    checks=[]; runs=[]
    for d,x in ((41,.90),(100,.99)):
        for y in ANGLES:
            checks.append(quality("native-gpu",4,d,y,x))
        for repeat in range(3):
            _,r=capture("native-gpu",4,d,132,x,repeat)
            runs.append(dict(trees=4,d=d,error=x,repeat=repeat,**r))
    write_csv(OUT/"grove-guards.csv",checks)
    write_csv(OUT/"grove-guard-timing.csv",runs)
    stock=[quality("production",n,d,132,1.) for n in (1,4) for d in (41,100)]
    write_csv(OUT/"stock-one-pixel.csv",stock)


def diagnostics():
    settings=frozen_settings()
    costs=[]
    for n in (1,4):
        for d in (24,41,100):
            x=settings[f"{n}/{d}/native"]
            for arm in ("native-gpu","native-packed"):
                _,r=capture(arm,n,d,132,x,"counters",True)
                costs.append(dict(trees=n,d=d,arm=arm,error=x,**r))
    write_csv(OUT/"pipeline.csv",costs)
    rows=[]
    for d,limits in ((24,(.85,.7,.5,.001)),(41,(.95,.90)),(100,(.99,.97))):
        for x in limits: rows.append(quality("native-gpu",4,d,197,x))
    write_csv(OUT/"holdout-diagnostics.csv",rows)


def summarize():
    with (OUT/"timings.csv").open() as f: d3d=list(csv.DictReader(f))
    with (OUT/"game-profiles.csv").open() as f: ue=list(csv.DictReader(f))
    result=[]
    for n in (1,4):
        for d in (24,41,100):
            def case(r): return int(r["trees"])==n and int(r["d"])==d
            for arm in dict.fromkeys(r["arm"] for r in d3d):
                runs=[r for r in d3d if case(r) and r["arm"]==arm]
                assert len(runs)==3 and len({r["tris"] for r in runs})==1
                result.append(dict(trees=n,d=d,arm=arm,tris=int(runs[0]["tris"]),error=float(runs[0]["error"]),
                    tree_gpu_ms=median(float(r["gpu_asset_ms"]) for r in runs),
                    traversal_and_encode_ms=median(float(r["gpu_live_cut_ms"]) for r in runs),
                    camera_gpu_ms=median(float(r["gpu_camera_ms"]) for r in runs),
                    frame_gpu_ms=median(float(r["gpu_frame_ms"]) for r in runs),
                    frame_ms=median(float(r["fenced_frame_ms"]) for r in runs),
                    min_frame_ms=min(float(r["fenced_frame_ms"]) for r in runs),
                    max_frame_ms=max(float(r["fenced_frame_ms"]) for r in runs),
                    tree_draws=int(float(runs[0]["camera_tree_draws"])),submissions=int(float(runs[0]["camera_tree_submission_calls"]))))
            for mode in ("fenced","pipelined"):
                runs=[r for r in ue if case(r) and r["mode"]==mode]
                assert len(runs)==2
                field="fenced_frame_ms" if mode=="fenced" else "frame_ms"
                result.append(dict(trees=n,d=d,arm="Nanite-"+mode,tris=int(runs[0]["tris"]),error=1.,
                    tree_gpu_ms=median(float(r["gpu_camera_ms"]) for r in runs),traversal_and_encode_ms=None,
                    camera_gpu_ms=None,frame_gpu_ms=median(float(r["gpu_frame_ms"]) for r in runs),
                    frame_ms=median(float(r[field]) for r in runs),min_frame_ms=min(float(r[field]) for r in runs),
                    max_frame_ms=max(float(r[field]) for r in runs),tree_draws=None,submissions=None))
    write_csv(OUT/"summary.csv",result)
    # Use actual shaded timing captures, retaining each camera's original framing.
    sheet=Image.new("RGB",(3*480,2*302),(28,30,32))
    draw=ImageDraw.Draw(sheet)
    font=ImageFont.truetype("segoeui.ttf",17)
    errors=frozen_settings()
    for row,n in enumerate((1,4)):
        for col,d in enumerate((24,41,100)):
            p,_=capture("native-packed",n,d,132,errors[f"{n}/{d}/native"],0)
            with Image.open(p) as im:
                sheet.paste(im.convert("RGB").resize((480,270),Image.Resampling.LANCZOS),(480*col,302*row+32))
            draw.text((480*col+8,302*row+4),f"{n} {'oak' if n==1 else 'oaks'}   {d} m",font=font,fill="white")
    sheet.save(OUT/"matched-views.jpg",quality=92)
    shaded=[]
    for n in (1,4):
        for d in (24,41,100):
            for family in ("native","canopy1"):
                x=errors[f"{n}/{d}/{family}"]
                cpu,_=capture(family+"-cpu",n,d,132,x,0)
                with Image.open(cpu) as im: control=np.asarray(im.convert("RGB"),dtype=np.int16)
                for suffix in ("-gpu","-packed"):
                    path,_=capture(family+suffix,n,d,132,x,0)
                    with Image.open(path) as im:
                        diff=np.abs(np.asarray(im.convert("RGB"),dtype=np.int16)-control).max(axis=2)
                    shaded.append(dict(trees=n,d=d,arm=family+suffix,pixels_over_one_code=int((diff>1).sum()),
                                       max_code=int(diff.max()),mean_pixel_max=float(diff.mean())))
    write_csv(OUT/"shaded-agreement.csv",shaded)


if __name__=="__main__":
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument("action",choices=("quality","timing","suite","verify","final","motion","summary","diagnostics","guards","view","stage"))
    p.add_argument("--trees",type=int,choices=(1,4),default=4)
    p.add_argument("--distance",type=int,choices=(24,41,100),default=41)
    p.add_argument("--yaw",type=int,choices=ANGLES,default=132)
    p.add_argument("--pixel-error",type=float)
    p.add_argument("--arm",choices=["production"]+[a+b for a in FAMILIES[1:] for b in ("-gpu","-cpu")]+["canopy1-packed","native-packed"],default="native-gpu")
    a=p.parse_args()
    if a.action in ("view","stage"):
        errors=frozen_settings()
        x=errors.get(f"{a.trees}/{a.distance}/{a.arm.split('-')[0]}",1)
        if a.pixel_error is not None: x=a.pixel_error
        exe,args=launch_args(a.arm,a.trees,a.distance,a.yaw,x,probe_gi=None)
        cmd=[str(exe),"--review-capture",*args]
        print(subprocess.list2cmdline(cmd))
        if a.action=="view": subprocess.Popen(cmd)
    else:
        if a.action in ("quality","suite"): qualify()
        if a.action in ("verify","final","suite"): verify_frozen()
        if a.action in ("motion","suite","final"): motion()
        if a.action in ("timing","suite","final"): timings()
        if a.action in ("diagnostics","suite","final"): diagnostics()
        if a.action in ("guards","suite","final"): check_guards()
        native_profiles()
        if a.action in ("timing","suite","final","summary"): summarize()
