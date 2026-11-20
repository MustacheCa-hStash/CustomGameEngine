"""Measure exposed-bough material, depth, lighting, shadows and moving-camera errors."""
import csv
import hashlib
import json
import subprocess
from pathlib import Path

import numpy as np
from PIL import Image
from BenchmarkOakBoughVoxels import ROOT, UE, TREE, stage_controls, shot, write_csv
from CompareOakMaskEdges import read_mask, measure

def pixels(path):
    with Image.open(path) as im:return np.asarray(im.convert("RGB"))

def decoded(path):
    r,g,b=pixels(path).astype(np.uint32).transpose(2,0,1)
    return (r+256*g+65536*b)*(1000/16777215)

def stills(views,labels):
    rows=[];arrays={};shadow={}
    for label,view,bake in [("source","ref-view",None),("triangles","pilot-view",None),("nanite-vox","native-view","native")]+[
        (l,"pilot-view",l) for l in labels]:
        v=views[view]
        dep,_=shot(v,"depth-"+label,bake,depth=True)
        mat,_=shot(v,"materials-"+label,bake,extra=["--bough-materials"])
        # Measure actual shaded output on the common visible material pixels.
        shaded,_=shot(v,"lit-"+label,bake,shaded=True)
        light,count=shot(v,"low-sun-"+label,bake,shaded=True,
            extra=["--lighting","shadow-visibility","--sun-elevation","25","--sun-azimuth","-70"])
        rgb=pixels(mat);labels2=np.where(rgb[:,:,1]>rgb[:,:,0],1,np.where(rgb[:,:,0]>rgb[:,:,1],2,0))
        z=decoded(dep);lum=pixels(shaded)@np.array([.2126,.7152,.0722])
        arrays[label]=(z,labels2,lum);shadow[label]=pixels(light)[:,:,0]
        zr,mr,lr=arrays["source"];common=(labels2>0)&(mr>0)
        q=dict(cut=label,depth_p95_mm=np.percentile(abs(z[common]-zr[common])*1000,95))
        for slot,name in [(1,"leaf"),(2,"twig")]:
            a=labels2==slot;b=mr==slot;shared=a&b
            q[name+"_pixels"]=int(a.sum());q[name+"_iou"]=np.count_nonzero(a&b)/np.count_nonzero(a|b)
            q[name+"_luma_delta_255"]=np.mean(lum[shared]-lr[shared])
        rows.append(q)
    for r in rows:
        label=r["cut"];empty=(arrays["triangles"][1]==0)&(arrays[label][1]==0)
        delta=np.abs(shadow[label].astype(float)-shadow["triangles"])
        r["off_geometry_shadow_different_pixels"]=np.count_nonzero((delta>1)&empty)
    write_csv(ROOT/"depth-material-shadow.csv",rows)
    native=arrays["nanite-vox"][0][850:1270,1480:2040]
    ue=np.fromfile(ROOT/"ue-depth-m.f32","<f4").reshape(native.shape)
    shared=(native>80)&(native<115)&(ue>80)&(ue<115)
    (ROOT/"replay-depth.json").write_text(json.dumps(dict(shared=int(shared.sum()),
        p50_p95_p99_mm=np.percentile(abs(native[shared]-ue[shared])*1000,[50,95,99]).tolist())))

def sequence(v,bake,start,end,wind):
    frames=60;fps=30
    files=[v/"CustomGameEngine.exe",v/"assets"/TREE]+([ROOT/(bake+".oakvx")] if bake else [])
    stamp=hashlib.sha256("".join(hashlib.file_digest(f.open("rb"),"sha256").hexdigest() for f in files).encode()).hexdigest()[:10]
    folder=ROOT/f"motion-{v.name}-{bake or 'tri'}-{start}-{end}-{wind}-{stamp}"
    args=[v/"CustomGameEngine.exe","--capture",folder,"--scene","forked-oak","--camera","overview",
        "--width","3840","--height","2160","--msaa","1","--distance",str(start),"--end-distance",str(end),
        "--yaw-degrees","132","--pitch-degrees","-2.235" if start==100 else "-5.46",
        "--end-pitch-degrees","-2.235" if end==100 else "-5.46","--wind-strength",str(wind),
        "--wind-time","0" if start==41 else str((frames-1)/fps),"--frames",str(frames),"--fps",str(fps),
        "--lod","auto","--spray-mode","hierarchy","--tree-submit","direct","--spray-culling","on",
        "--spray-local-lod","on","--spray-pixel-error","1","--probe-gi","off","--geometry-mask","--cull-spray-wood-backfaces"]
    if bake:args+=["--bough-voxel-bake",files[-1]]
    if wind:args+=["--animate-wind"]
    if not (folder/"frames.csv").exists():
        with folder.with_suffix(".log").open("w") as log:subprocess.run(args,stdout=log,stderr=log,check=True,timeout=240)
    return folder

def motion(views,partial):
    rows=[]
    for wind in (0,1):
        for start,end in [(41,100),(100,41)]:
            paths=[sequence(views["ref-view"],None,start,end,wind),sequence(views["pilot-view"],None,start,end,wind),
                   sequence(views["pilot-view"],partial,start,end,wind)]
            last=None
            for i in range(60):
                masks=[read_mask(p/f"frame-{i:06}.png")[380:1650,800:2950] for p in paths]
                if last is None:last=masks
                for label,m,previous in zip(("triangles",partial),masks[1:],last[1:]):
                    q=next(measure(masks[0],[(0,0,m)]))
                    delta=(m^previous)^(masks[0]^last[0])
                    rows.append(dict(wind=wind,direction=f"{start}-{end}",frame=i,distance=start+(end-start)*i/59,
                        cut=label,iou=q[2],edge=q[-1],transition_delta_pixels=int(delta.sum()),
                        union_pixels=int((m|masks[0]|previous|last[0]).sum())))
                last=masks
            write_csv(ROOT/"motion.csv",rows)

if __name__=="__main__":
    import sys
    labels=list(dict.fromkeys(json.loads((ROOT/"finalists.json").read_text())))
    views=stage_controls()
    if "motion" not in sys.argv:stills(views,labels)
    if "still" not in sys.argv:motion(views,next(l for l in labels if l.startswith("partial-")))
