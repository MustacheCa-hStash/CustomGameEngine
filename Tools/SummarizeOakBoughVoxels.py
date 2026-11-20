"""Collect measured bough work, native profiles, paired GPU medians and image gates."""
import csv
import json
import struct
from statistics import median

import numpy as np
from PIL import Image
from DecodeOakBoughVoxels import ROOT, UE, write_csv

def read(name):
    with (ROOT/name).open() as f:return list(csv.DictReader(f))

def work():
    rows=[]
    for trial in read("screen.csv"):
        label=trial["cut"]
        path=ROOT/(("native" if label=="nanite-vox" else label)+".oakvx")
        cells=bricks=payload=alloc=0
        sizes=[]
        if path.exists():
            with path.open("rb") as f:
                magic,first,count,mask=struct.unpack("<8sIII",f.read(20));assert magic==b"OAKVX001"
                for slot in range(2):
                    nb,nc=struct.unpack("<II",f.read(8))
                    allocations=(nb*24,nb*64,4*nb+16*nc)
                    cells+=nc;bricks+=nb;payload+=sum(allocations)
                    alloc+=sum(((a+65535)//65536)*65536 for a in allocations)
                    f.seek(sum(allocations),1);sizes.append((nb,nc))
                assert not f.read(1)
        shot=ROOT/"captures"/f"screen-{label}.png"
        a=np.asarray(Image.open(shot).convert("RGB"))
        vp=int(np.count_nonzero(a[:,:,1]>a[:,:,2]))
        assert bricks==int(trial["bricks"])
        rows.append(dict(cut=label,source_tris=trial["source_tris"],box_tris=bricks*12,total_tris=int(trial["source_tris"])+bricks*12,
            bricks=bricks,cells=cells,leaf_cells=sizes[0][1] if sizes else 0,twig_cells=sizes[1][1] if sizes else 0,
            camera_voxel_pixels=vp,bake_bytes=path.stat().st_size if path.exists() else 0,
            buffer_bytes=payload,estimated_committed_buffer_bytes=alloc))
    write_csv(ROOT/"work.csv",rows)

def profiles():
    rows=[]
    for f in sorted((UE.parent/"Saved/Profiling/CSV").glob("vx7-shaded-100-*.csv")):
        a=[]
        with f.open(encoding="utf-8-sig") as src:
            for r in csv.DictReader(src):
                try:a.append(tuple(float(r[k]) for k in ("GPU/NaniteVisBuffer","GPU/NaniteBasePass")))
                except (TypeError,ValueError,KeyError):continue
        assert len(a)==65;b=a[8:-5]
        rows.append(dict(run=f.stem,samples=len(b),vis_ms=median(x for x,y in b),base_ms=median(y for x,y in b),
            vis_plus_base_ms=median(x+y for x,y in b)))
    assert len(rows)==4;write_csv(ROOT/"native-profile.csv",rows)
    a=read("timings.csv");out=[]
    for view,label in dict.fromkeys((r["view"],r["cut"]) for r in a):
        runs=[r for r in a if r["view"]==view and r["cut"]==label];assert len(runs)==4
        ref=[r for r in a if r["cut"]=="triangles" and (r["view"]=="crown-view")==(view=="crown-view")]
        base={r["run"]:float(r["gpu_camera_ms"]) for r in ref}
        row=dict(view=view,cut=label,source_tris=runs[0]["source_tris"],bricks=runs[0]["bricks"])
        for key in ("gpu_camera_ms","gpu_shadow_ms","gpu_frame_ms"):
            v=[float(r[key]) for r in runs]
            row.update({key:median(v),key+"_min":min(v),key+"_max":max(v)})
        delta=[float(r["gpu_camera_ms"])-base[r["run"]] for r in runs]
        row.update(paired_camera_delta_ms=median(delta),delta_min=min(delta),delta_max=max(delta))
        out.append(row)
    write_csv(ROOT/"cost.csv",out)

def motion():
    a=read("motion.csv");rows=[]
    for wind,dire,label in dict.fromkeys((r["wind"],r["direction"],r["cut"]) for r in a):
        part=[r for r in a if (r["wind"],r["direction"],r["cut"])==(wind,dire,label)]
        assert len(part)==60
        rows.append(dict(wind=wind,direction=dire,cut=label,frames=len(part),min_iou=min(float(r["iou"]) for r in part),
            max_edge=max(float(r["edge"]) for r in part),p95_transition_delta_fraction=np.percentile([
            int(r["transition_delta_pixels"])/int(r["union_pixels"]) for r in part[1:]],95)))
    write_csv(ROOT/"motion-summary.csv",rows)

if __name__=="__main__":
    work();profiles();motion()
