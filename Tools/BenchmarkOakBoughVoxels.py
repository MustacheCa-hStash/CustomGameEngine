"""Stage the selected cut and benchmark frozen exposed-bough camera fixtures."""
import argparse
import csv
import json
from pathlib import Path
import shutil
import struct
import subprocess
import uuid
from statistics import median

import numpy as np
from DecodeOakBoughVoxels import ROOT, write_csv
from ReplayOakAssembly import BUILD, OUT, TREE, UE, Hierarchy, mesh, quality, read_mask, reference
from PackageReusableOak import layout
from ScoreReusableOak import ue_images
from CompareOakMaskEdges import measure

BASE=OUT/"reusable"
def stage(name,base,residual=False):
    from ReplayOakAssembly import viewer_assets
    viewer=viewer_assets(str(ROOT/name),BASE/base)
    src=BASE/base/"assets"/TREE
    if not residual:
        shutil.copyfile(src,viewer/"assets"/TREE)
    else:
        h=Hierarchy(src.read_bytes())
        geometry=ROOT/"native.mesh" if residual is True else residual
        v,ix=mesh(geometry); mat=np.fromfile(geometry.with_suffix(".mat"),"u1")
        for slot,part in enumerate(h.parts[:2]):
            n=part[1]; first=layout()[7][0][0]
            route=next(j for j in range(len(n)//56) if struct.unpack_from("<II",n,56*j+48)==(first,371)
                       and struct.unpack_from("<I",n,56*j+44)[0]==2)
            child=struct.unpack_from("<I",n,56*route+40)[0]
            struct.pack_into("<ff",n,56*route+32,0,0)
            faces=ix[mat==slot]; begin=len(part[3])//4; count=faces.size
            if count:
                part[3]+=(faces+len(part[2])//40).astype("<u4").tobytes();part[2]+=v.tobytes()
            else:part[3]+=bytes(12);count=3
            struct.pack_into("<IIff",n,56*child+24,begin,count,0.,0.)
        h.write(viewer/"assets"/TREE)
    return viewer

def shot(view,name,bake=None,yaw=132,d=100,shaded=False,crown=False,depth=False,wind=0,phase=0,budget=1,extra=()):
    dest=ROOT/"captures";dest.mkdir(exist_ok=True)
    path=dest/(name+".png")
    args=[view/"CustomGameEngine.exe","--capture",path,"--scene","forked-oak","--camera","overview",
          "--width","3840","--height","2160","--msaa","1","--distance",str(d),"--yaw-degrees",str(yaw),
          "--pitch-degrees","-2.235" if d==100 else "-5.46","--wind-strength",str(wind),"--wind-time",str(phase),
          "--lod","auto","--spray-mode","hierarchy","--tree-submit","direct","--spray-culling","on",
          "--spray-local-lod","on","--spray-pixel-error",str(budget),"--lighting","combined","--probe-gi","off",
          "--timing-samples","61" if shaded else "3"]
    if not crown:args+=["--cull-spray-wood-backfaces"]
    if not shaded:args+=["--geometry-mask"]
    if depth:args+=["--bough-depth"]
    if bake:args+=["--bough-voxel-bake",ROOT/(bake+".oakvx")]
    args.extend(extra)
    pending=path.with_name(f"{name}-{uuid.uuid4().hex}.png");args[2]=pending
    with path.with_suffix(".log").open("w") as f:
        subprocess.run(args,check=True,stdout=f,stderr=f,timeout=90)
    pending.replace(path)
    pending.with_suffix(".timings.csv").replace(path.with_suffix(".timings.csv"))
    rows=list(csv.DictReader(path.with_suffix(".timings.csv").open()))
    bricks=int(rows[0]["camera_voxel_bricks"])
    return path,dict(bricks=bricks,source_tris=sum(int(rows[0][k]) for k in ("camera_leaf_triangles","camera_spray_wood_triangles","camera_tree_trunk_triangles"))-12*bricks-(0 if crown else 5),
        **{k:median(float(r[k]) for r in rows) for k in ("gpu_camera_ms","gpu_shadow_ms","gpu_frame_ms")})

def native_check():
    v=stage("native-view","pilot-7",True)
    p,c=shot(v,"native-mask","native")
    score=quality(p,read_mask(UE/"vx7-100-Mask.png",True))
    print("native replay",score,c,flush=True)
    p,_=shot(v,"native-depth","native",depth=True)

def review():
    from PIL import Image,ImageDraw,ImageFont
    v=stage_controls()
    ref,_=shot(v["ref-view"],"review-source")
    reference=read_mask(ref);yy,xx=np.where(reference)
    box=(int(xx.min()-5),int(yy.min()-5),int(xx.max()+6),int(yy.max()+6))
    labels=[("Current triangles","pilot-view",None),("Nanite bricks in D3D","native-view","native"),
            ("Fitted leaves, 3.8 cm","pilot-view","merged-38-fit-leaf"),
            ("One group, 5.8 cm","pilot-view","partial-58-joint")]
    canvas=Image.new("RGB",(1260,670),(22,25,29));draw=ImageDraw.Draw(canvas)
    font=ImageFont.truetype("C:/Windows/Fonts/segoeui.ttf",18)
    draw.text((20,8),"Exposed white oak bough 7 at 100 m",font=font,fill="white")
    draw.text((400,8),"Grey  shared coverage      Cyan  missing from cut      Orange  extra in cut",font=font,fill=(210,218,222))
    for j,(title,view,bake) in enumerate(labels):
        p,c=shot(v[view],"review-"+view+"-"+str(bake),bake)
        m=read_mask(p);x=j%2*630+20;y=j//2*307+48
        image=np.full((*m.shape,3),[22,25,29],dtype="u1")
        image[m&reference]=[163,174,176];image[~m&reference]=[80,207,241];image[m&~reference]=[253,154,73]
        sample=Image.fromarray(image).crop(box)
        factor=min(595/sample.width,243/sample.height)
        canvas.paste(sample.resize((round(sample.width*factor),round(sample.height*factor)),Image.Resampling.NEAREST),(x,y+54))
        draw.text((x,y),title,fill="white",font=font)
        draw.text((x,y+25),f'{c["source_tris"]:,} surface triangles    {c["bricks"]:,} bricks    IoU {scores(p,reference)["iou"]:.3f}',
                  fill=(181,191,197),font=font)
    path=ROOT/"comparison.png";canvas.save(path);print(path)

def stage_controls():
    return {k:stage(k,b,residual) for k,b,residual in (
        ("pilot-view","pilot-7",False),("ref-view","ref-7",False),("native-view","pilot-7",True),
        ("triangle-replay","pilot-7",BASE/"live7-100.mesh"),("crown-view","pilot",False))}

def scores(path,reference,ue=False):
    m=next(measure(reference,[(0,0,read_mask(path,ue))]))
    return dict(iou=m[2],edge=m[-1],missing=int(m[3]),extra=int(m[4]))

def screen(partial_only=False):
    v=stage_controls()
    ref,_=shot(v["ref-view"],"ref-132")
    ref=read_mask(ref)
    rows=list(csv.DictReader((ROOT/"screen.csv").open())) if partial_only else []
    if partial_only:rows=[r for r in rows if not r["cut"].startswith("partial-")]
    controls=[] if partial_only else [("triangles","pilot-view",None),("nanite-tri","triangle-replay",None),("nanite-vox","native-view","native")]
    for label,view,bake in controls+[
        (p.stem,"pilot-view",p.stem) for p in sorted(ROOT.glob("partial-*.oakvx" if partial_only else "*.oakvx")) if p.stem!="native"]:
        path,work=shot(v[view],f"screen-{label}",bake)
        rows.append(dict(cut=label,**work,**scores(path,ref)))
        write_csv(ROOT/"screen.csv",rows)
        print(label,work["source_tris"],work["bricks"],round(rows[-1]["iou"],4),round(rows[-1]["edge"],4),flush=True)

def finish(reuse=False):
    v=stage_controls()
    rows=list(csv.DictReader((ROOT/"screen.csv").open()))
    cases=[r for r in rows if r["cut"].startswith(("merged-","part-"))]
    # Pick the highest reference IoU for budgeted leaf-only and joint cases separately.
    best=[max([r for r in cases if r["cut"].endswith(k) and "-free-" not in r["cut"]],key=lambda r:float(r["iou"]))["cut"] for k in ("leaf","joint")]
    free=max([r for r in cases if "-free-" in r["cut"]],key=lambda r:float(r["iou"]))["cut"]
    labels=list(dict.fromkeys([*best,free]))
    tri=next(r for r in rows if r["cut"]=="nanite-tri")
    partial=[r for r in rows if r["cut"].startswith("partial") and float(r["iou"])>=float(tri["iou"]) and float(r["edge"])<=float(tri["edge"])]
    if partial:
        chosen=min(partial,key=lambda r:int(r["source_tris"])+12*int(r["bricks"]))
        labels.append(chosen["cut"])
    (ROOT/"finalists.json").write_text(json.dumps(labels))
    angles=(132,90,180,45,225,0,270,68,315,302,330,17,204)
    qual=[]
    for yaw in angles:
        ref=ROOT/"captures"/f"ref-{yaw}.png"
        if not reuse or not ref.exists():ref,_=shot(v["ref-view"],f"ref-{yaw}",yaw=yaw)
        ref=read_mask(ref)
        suffix="" if yaw==132 else f"-y{yaw}"
        ug=scores(UE/f"vx7-100{suffix}-Mask.png",read_mask(UE/f"vx7-100{suffix}-Fine.png",True),True)
        p,f=ue_images(7,100,yaw)
        tg=scores(p,read_mask(f,True),True) if p.exists() and f.exists() else None
        for label in ["triangles",*labels]:
            path=ROOT/"captures"/f"validate-{label}-{yaw}.png"
            if reuse and path.exists():
                timing=list(csv.DictReader(path.with_suffix(".timings.csv").open()))
                row=timing[0];bricks=int(row["camera_voxel_bricks"])
                c=dict(bricks=bricks,source_tris=sum(int(row[k]) for k in ("camera_leaf_triangles","camera_spray_wood_triangles","camera_tree_trunk_triangles"))-12*bricks-5,
                    **{k:median(float(r[k]) for r in timing) for k in ("gpu_camera_ms","gpu_shadow_ms","gpu_frame_ms")})
            else:path,c=shot(v["pilot-view"],f"validate-{label}-{yaw}",None if label=="triangles" else label,yaw=yaw)
            q=scores(path,ref)
            qual.append(dict(yaw=yaw,cut=label,**q,**c,voxel_gate_iou=ug["iou"],voxel_gate_edge=ug["edge"],
                triangle_gate_iou=tg["iou"] if tg else "",triangle_gate_edge=tg["edge"] if tg else "",
                passes_vox=q["iou"]>=ug["iou"] and q["edge"]<=ug["edge"],
                passes_tri=(q["iou"]>=tg["iou"] and q["edge"]<=tg["edge"]) if tg else ""))
        write_csv(ROOT/"validation.csv",qual)
    crown_budget=json.loads((BASE/"crown-chosen.json").read_text())["pilot-100"]["budget"]
    whole=reference(100,whole=True);crown=[]
    for label in ["triangles",*labels]:
        path,work=shot(v["crown-view"],f"crown-{label}",None if label=="triangles" else label,crown=True,budget=crown_budget)
        crown.append(dict(cut=label,**scores(path,whole),**work))
    write_csv(ROOT/"crown.csv",crown)
    timings(v,labels)

def timings(v,labels):
    crown_budget=json.loads((BASE/"crown-chosen.json").read_text())["pilot-100"]["budget"]
    timed=[("triangles","pilot-view",None),("nanite-tri","triangle-replay",None),("nanite-vox","native-view","native")]
    timed += [(label,"pilot-view",label) for label in labels]
    timed += [("triangles","crown-view",None)]+[(label,"crown-view",label) for label in labels]
    data=[]
    for run in range(4):
        for label,view,bake in (timed if run%2==0 else reversed(timed)):
            crown=view=="crown-view"
            _,c=shot(v[view],f"time-{view}-{label}-{run}",bake,shaded=True,crown=crown,budget=crown_budget if crown else 1)
            data.append(dict(run=run,view=view,cut=label,**c))
        write_csv(ROOT/"timings.csv",data)

if __name__=="__main__":
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument("action",choices=["native","screen","partial","finish","complete","timing","review"],default="native",nargs="?")
    {"native":native_check,"screen":screen,"partial":lambda:screen(True),"finish":finish,
     "complete":lambda:finish(True),"timing":lambda:timings(stage_controls(),json.loads((ROOT/"finalists.json").read_text())),
     "review":review}[parser.parse_args().action]()
