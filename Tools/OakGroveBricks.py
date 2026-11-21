"""Compare one 371-spray bough per oak with four trees on a continuous camera path."""
import argparse
import csv
import json
from pathlib import Path
import struct
import subprocess
import tempfile
import time
from statistics import median

import numpy as np
from PIL import Image
from BenchmarkOakGpuTraversal import native_counter
from CompareOakMaskEdges import measure,read_mask
from DecodeOakBoughVoxels import ROOT as BRICKS,write_csv
from MixedOakSpray import flatten,unreal
from MixedOakSpray import flags as spray_flags
from BenchmarkOakGpuTraversal import launch_args
from BenchmarkOakLiveCuts import command as live_command
from PackageReusableOak import layout
from ReplayOakAssembly import BUILD,REPO,UE,TREE,Hierarchy,viewer_assets,mesh

OUT=BUILD/"oak-grove-bricks"
PARTS=BUILD/"oak-assembly-step/reusable"
SEGMENTS=((50,100),(100,175))
STEPS=26


def native(hzb=False):
    jobs=[]
    for start,end in SEGMENTS:
        arms=[] if hzb else [("fine","Triangles","Mask",.001)]
        arms.extend([("tri","Triangles","Mask",1),("vox","Voxels","Mask",1),
                     ("tri-count","Triangles","OffscreenStats",1),("vox-count","Voxels","OffscreenStats",1)])
        for repeat,order in enumerate((("tri","vox"),("vox","tri"))):
            arms.extend((f"{arm}-gpu-{repeat}","Triangles" if arm=="tri" else "Voxels","Profile",1) for arm in order)
        for arm,asset,mode,error in arms:
            name="Tree_Optimized_"+asset
            prefix="grove-hzb" if hzb else "grove-bricks"
            jobs.append(dict(name=f"{prefix}-{start}-{end}-{arm}",asset=name,
                mesh_path="/Game/NaniteOakBench/"+name,mode=mode,max_pixels_per_edge=error,steps=STEPS,
                distance=start,end_distance=end,trees=4,yaw=132,pitch=-2.235,frustum=1,hzb=int(hzb),
                resolution=[3840,2160],vertical_fov=45,isolated=True,capture_only=True))
    file=OUT/("hzb-jobs.json" if hzb else "jobs.json");file.write_text(json.dumps(jobs))
    started=time.time_ns()
    unreal("CaptureMixedOakMotion.py","native-hzb-motion" if hzb else "native-motion","OAK_CAPTURE CAPTURES_COMPLETE",dict(
        OAK_MIXED_DIR=str(OUT),OAK_GROVE_CAPTURE_JOBS=str(file),
        OAK_GROVE_MOTION_RIG=str(REPO/"Tools/CaptureOakGpuComparison.py")),OUT)
    for job in jobs:
        name=job["name"]
        paths=([UE.parent/"Saved/Profiling/CSV"/(name+".csv")] if job["mode"]=="Profile" else
               [OUT/"native"/f"{name}-{i:03}.png" for i in range(2*STEPS)])
        if any(p.stat().st_mtime_ns<started for p in paths):raise RuntimeError(f"Incomplete captures for {name}")


def stage():
    source=PARTS/"pilot"
    dest=viewer_assets(str(OUT/"viewer"),source)
    h=Hierarchy((source/"assets"/TREE).read_bytes())
    sizes=[4]*len(h.parts)+[1]*len(h.parts)
    arrays=[]
    at=0
    for size in sizes:
        n,=struct.unpack_from("<I",h.tail,at)
        arrays.append(h.tail[at+4:at+4+n*size])
        at+=4+n*size
    assert at==len(h.tail)
    v,faces=mesh(BRICKS/"native.mesh")
    material=np.fromfile(BRICKS/"native.mat","u1")
    payload=bytearray((BRICKS/"native.oakvx").read_bytes())
    magic,first,count,replace=struct.unpack_from("<8sIII",payload)
    groups=layout()[7]
    assert (magic,first,count,replace)==(b"OAKVX001",groups[0][0],sum(len(g[1]) for g in groups),0)
    struct.pack_into("<8sIII",payload,0,b"OAKMX001",first,count,3)
    with tempfile.NamedTemporaryFile(dir=OUT,delete=False) as f:
        f.write(payload)
        pending=Path(f.name)
    pending.replace(OUT/"bough7.oakvx")
    for slot,part in enumerate(h.parts[:2]):
        raw=part[1]
        kinds=arrays[len(h.parts)+slot]
        target,=[i for i,kind in enumerate(kinds) if kind==2 and struct.unpack_from("<II",raw,i*56+48)==(first,count)]
        used,indices=np.unique(faces[material==slot],return_inverse=True)
        begin=len(part[3])//4
        part[3]+=(indices.ravel()+len(part[2])//40).astype("<u4").tobytes()
        part[2]+=v[used].tobytes()
        def visit(i):
            n=raw[56*i:56*(i+1)]
            child,num=struct.unpack_from("<II",n,40)
            original=(n,kinds[i],[visit(k) for k in range(child,child+num)])
            if i!=target:return original
            residual=n[:24]+struct.pack("<IIffII",begin,indices.size,0,0,0,0)+n[48:]
            route=n[:24]+struct.pack("<IIff",0,0,.026,.026)+n[40:]
            return (route,3,[(residual,0,[]),original])
        part[1],tags=flatten(visit(0))
        arrays[len(h.parts)+slot]=bytes(tags)
    h.tail=b"".join(struct.pack("<I",len(a)//size)+a for a,size in zip(arrays,sizes))
    with tempfile.NamedTemporaryFile(dir=dest/"assets",delete=False) as f:pending=Path(f.name)
    try:
        h.write(pending)
        pending.replace(dest/"assets"/TREE)
    finally:
        pending.unlink(missing_ok=True)
    return dest/"CustomGameEngine.exe"


def command(arm,distance):
    if arm=="fine-depth":
        exe,args=live_command("crown","source",distance,132,probe_gi=None)
        args+=["--tree-count","4"]
        return exe,args
    if arm in ("gpu-tri","gpu-qualified"):
        exe,args=launch_args("native-gpu",4,distance,132,.90 if arm=="gpu-qualified" else 1.,probe_gi=None)
        args[args.index("--pitch-degrees")+1]="-2.235"
        return exe,args
    args=spray_flags("tri",distance)
    args.remove("--cull-spray-wood-backfaces")
    args+=["--tree-count","4"]
    if arm!="tri":args+=["--bough-voxel-bake",str(OUT/"bough7.oakvx")]
    if arm=="compute":args+=["--brick-raster"]
    return OUT/"viewer/CustomGameEngine.exe",args


def d3d(arms,types):
    for mode in types:
        for repeat in range(3 if mode=="gpu" else 1):
            order=arms[repeat:]+arms[:repeat]
            for arm in order:
                for start,end in SEGMENTS:
                    for leg,(a,b) in enumerate(((start,end),(end,start))):
                        folder=OUT/f"{mode}-{arm}-{start}-{end}-{repeat}-{leg}"
                        exe,args=command(arm,a)
                        flags=["--sequence-timings-only"] if mode=="gpu" else ["--geometry-mask","--probe-gi","off"]
                        with tempfile.TemporaryDirectory(dir=OUT) as tmp:
                            path=Path(tmp)/"frames"
                            with (OUT/"capture.log").open("a") as log:
                                subprocess.run([exe,"--capture",path,*args,"--frames",str(STEPS),"--end-distance",str(b),
                                                "--fps","30",*flags],stdout=log,stderr=log,check=True,timeout=300)
                            with (path/"frames.csv").open() as f:rows=list(csv.DictReader(f))
                            if len(rows)!=STEPS or abs(float(rows[-1]["distance"])-b)>.01:raise ValueError(folder)
                            if mode=="mask" and len(list(path.glob("frame-*.png")))!=STEPS:raise ValueError(folder)
                            folder.mkdir(exist_ok=True)
                            for p in path.iterdir():p.replace(folder/p.name)
                print(mode,repeat,arm,flush=True)


def rows(file):
    with file.open() as f:return list(csv.DictReader(f))


def tile_coverage(mask):
    h,w=mask.shape
    return mask.reshape(h//8,8,w//8,8).all(axis=(1,3))


def verify_depth():
    results=[]
    occluders=[]
    for distance in (74,100,175):
        images=[]
        for arm in ("box","compute","tri","fine-depth"):
            exe,args=command(arm,distance)
            with tempfile.TemporaryDirectory(dir=OUT) as tmp:
                path=Path(tmp)/"depth.png"
                with (OUT/"depth.log").open("a") as log:
                    subprocess.run([exe,"--capture",path,*args,"--geometry-mask","--bough-depth","--probe-gi","off"],
                                   check=True,timeout=120,stdout=log,stderr=log)
                path.replace(OUT/f"depth-{arm}-{distance}.png")
            with Image.open(OUT/f"depth-{arm}-{distance}.png") as im:
                r,g,b=np.asarray(im.convert("RGB"),dtype=np.uint32).transpose(2,0,1)
                images.append(r+(g<<8)+(b<<16))
        box,compute,tri,fine=images
        area=(box!=tri)|(compute!=tri)
        shared=area&(box>0)&(compute>0)
        delta=np.abs(box[shared].astype(float)-compute[shared])*1e6/16777215
        results.append(dict(distance=distance,affected=int(area.sum()),common_hits=int(shared.sum()),
            occupancy_differences=int(np.count_nonzero((box>0)^(compute>0))),
            p95_mm=float(np.percentile(delta,95)),above_1mm=int((delta>1).sum()),max_mm=float(delta.max()),
            compute_nearer_over_1m=int(((delta>1000)&(compute[shared]<box[shared])).sum()),
            compute_farther_over_1m=int(((delta>1000)&(compute[shared]>box[shared])).sum())))
        for offset in (0,5,15):
            # Hypothetical screen-aligned 8x8 objects at a specified nearest depth.
            limit=(distance+offset)*16777215/1000
            oracle=tile_coverage((fine>0)&(fine<limit))
            for arm,depth in (("tri",tri),("compute",compute)):
                hidden=tile_coverage((depth>0)&(depth<limit))
                occluders.append(dict(distance=distance,probe_depth=distance+offset,arm=arm,culled=int(hidden.sum()),
                    unsafe=int((hidden&~oracle).sum()),missed=int((~hidden&oracle).sum()),oracle=int(oracle.sum())))
    write_csv(OUT/"depth.csv",results)
    write_csv(OUT/"depth-occlusion.csv",occluders)


ARMS=["tri","compute","box","gpu-tri","gpu-qualified"]


def score():
    quality=[]
    times=[]
    for start,end in SEGMENTS:
        stem=f"grove-bricks-{start}-{end}"
        assets=["ue-vox","ue-tri",*ARMS]
        for leg in (0,1):
            counts={arm:rows(OUT/f"mask-{arm}-{start}-{end}-0-{leg}/frames.csv") for arm in ARMS}
            prev={}
            for k in range(STEPS):
                frame=leg*STEPS+k
                label=f"{frame:03}.png"
                ref=read_mask(OUT/"native"/f"{stem}-fine-{label}",True)
                tiles=tile_coverage(ref)
                pending=[]
                masks={}
                for arm in assets:
                    if arm.startswith("ue-"):
                        name=arm[3:]
                        path=OUT/"native"/f"{stem}-{name}-{label}"
                        stat=OUT/"native"/f"{stem}-{name}-count-{label}"
                        tris,bricks=native_counter(stat,424),native_counter(stat,456)
                        m=read_mask(path,True)
                    else:
                        c=counts[arm][k]
                        bricks=int(c["camera_voxel_bricks"])
                        tris=sum(int(c[key]) for key in ("camera_leaf_triangles","camera_spray_wood_triangles","camera_tree_trunk_triangles"))
                        if arm=="box":tris-=12*bricks
                        m=read_mask(OUT/f"mask-{arm}-{start}-{end}-0-{leg}/frame-{k:06}.png")
                    _,_,iou,_,_,edge=next(measure(ref,[(0,0,m)]))
                    full=tile_coverage(m)
                    distance=float(counts["tri"][k]["distance"])
                    changes=(np.count_nonzero((m^prev[arm])^(ref^prev["ref"]))/
                             np.count_nonzero(m|ref|prev[arm]|prev["ref"])) if prev else None
                    pending.append(dict(start=start,end=end,leg=leg,k=k,distance=distance,arm=arm,
                        triangles=tris,bricks=bricks,iou=iou,outline=edge,temporal=changes,
                        full_tiles=int(full.sum()),unsafe_tiles=int((full&~tiles).sum()),reference_tiles=int(tiles.sum())))
                    masks[arm]=m
                target=pending[0]
                for r in pending:
                    r["pass"]=bool(r["iou"]>=target["iou"] and r["outline"]<=target["outline"])
                agreement=np.count_nonzero(masks["compute"]&masks["box"])/np.count_nonzero(masks["compute"]|masks["box"])
                pending[assets.index("compute")]["box_iou"]=agreement
                for r in pending:r.setdefault("box_iou",None)
                quality.extend(pending)
                prev=dict(masks,ref=ref)
        for arm in ARMS:
            for repeat in range(3):
                for leg in (0,1):
                    for r in rows(OUT/f"gpu-{arm}-{start}-{end}-{repeat}-{leg}/frames.csv"):
                        asset,brick,camera,gpu=(float(r[key]) for key in ("gpu_asset_ms","gpu_bough_brick_ms","gpu_camera_ms","gpu_frame_ms"))
                        if min(asset,camera,gpu)<=0:raise ValueError("Missing GPU timestamps")
                        times.append(dict(start=start,arm=arm,repeat=repeat,distance=r["distance"],tree=asset+brick,
                                          brick=brick,camera=camera,frame=gpu))
        for arm in ("tri","vox"):
            for repeat in (0,1):
                data=rows(UE.parent/"Saved/Profiling/CSV"/f"{stem}-{arm}-gpu-{repeat}.csv")
                valid=[r for r in data if (r.get("FrameTime") or "").replace(".","",1).isdigit()]
                if len(valid)!=2*STEPS+5:raise ValueError("Incomplete Unreal timing series")
                for k,r in enumerate(valid[3:3+2*STEPS]):
                    d=start+(end-start)*min(k,2*STEPS-1-k)/(STEPS-1)
                    times.append(dict(start=start,arm="ue-"+arm,repeat=repeat,distance=d,
                                      tree=float(r["GPU/NaniteVisBuffer"])+float(r["GPU/NaniteBasePass"]),
                                      brick=None,camera=None,frame=float(r["GPUTime"])))
    write_csv(OUT/"quality.csv",quality)
    write_csv(OUT/"timings.csv",times)
    summary=[]
    for start,end in SEGMENTS:
        for arm in [*ARMS,"ue-tri","ue-vox"]:
            q=[r for r in quality if r["arm"]==arm and r["start"]==start]
            t=[r for r in times if r["arm"]==arm and r["start"]==start]
            record=dict(start=start,end=end,arm=arm,passed=sum(r["pass"] for r in q),poses=len(q))
            for name in ("triangles","bricks","iou","outline","temporal","full_tiles","unsafe_tiles"):
                record[name]=median(r[name] for r in q if r[name] is not None)
            for name in ("tree","brick","frame"):
                vals=[median(r[name] for r in t if r["repeat"]==rep) for rep in sorted(set(r["repeat"] for r in t)) if t[0][name] is not None]
                record[name]=median(vals) if vals else None
                record[name+"_runs"]=vals
            summary.append(record)
    (OUT/"summary.json").write_text(json.dumps(summary,indent=2))


def score_hzb():
    comparison=[]
    timing=[]
    for start,end in SEGMENTS:
        for arm in ("tri","vox"):
            for k in range(2*STEPS):
                rows_at_pose=[]
                for prefix in ("grove-bricks","grove-hzb"):
                    stem=f"{prefix}-{start}-{end}-{arm}"
                    counts=OUT/"native"/f"{stem}-count-{k:03}.png"
                    mask=read_mask(OUT/"native"/f"{stem}-{k:03}.png",True)
                    rows_at_pose.append((native_counter(counts,424),native_counter(counts,456),mask))
                a,b=rows_at_pose
                comparison.append(dict(start=start,arm=arm,frame=k,tri_off=a[0],tri_on=b[0],brick_off=a[1],
                    brick_on=b[1],lost=int((a[2]&~b[2]).sum()),extra=int((b[2]&~a[2]).sum()),
                    pixels=int(a[2].sum())))
            for prefix in ("grove-bricks","grove-hzb"):
                for repeat in (0,1):
                    records=rows(UE.parent/"Saved/Profiling/CSV"/f"{prefix}-{start}-{end}-{arm}-gpu-{repeat}.csv")
                    valid=[r for r in records if (r.get("FrameTime") or "").replace(".","",1).isdigit()]
                    if len(valid)!=2*STEPS+5:raise ValueError("Incomplete occlusion profile")
                    samples=valid[3:3+2*STEPS]
                    timing.append(dict(start=start,arm=arm,hzb=prefix=="grove-hzb",repeat=repeat,
                        vis_base=median(float(r["GPU/NaniteVisBuffer"])+float(r["GPU/NaniteBasePass"]) for r in samples)))
    write_csv(OUT/"occlusion.csv",comparison)
    write_csv(OUT/"occlusion-gpu.csv",timing)


if __name__=="__main__":
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument("action",choices=("stage","view","benchmark","score"))
    parser.add_argument("--arm",choices=ARMS,default="compute")
    parser.add_argument("--distance",type=int,default=100)
    options=parser.parse_args()
    OUT.mkdir(exist_ok=True)
    if options.action in ("view","stage","benchmark"):stage()
    if options.action=="view":
        exe,flags=command(options.arm,options.distance)
        subprocess.Popen([exe,"--review-capture",*flags])
    if options.action=="benchmark":
        native()
        d3d(ARMS,["mask","gpu"])
        native(True)
        verify_depth()
    if options.action in ("benchmark","score"):
        score()
        score_hzb()
