"""Bake and compare one exposed group of white-oak sprays on a 4K camera dolly."""
import argparse
import csv
import hashlib
import json
import os
import struct
import subprocess
import tempfile
import time
from pathlib import Path
from statistics import median

import numpy as np
from PIL import Image

from CompareOakMaskEdges import measure, outline, read_mask
from DecodeOakBoughVoxels import write_bake, write_csv
from FitOakBoughVoxels import EXE, rays, view
from PackageReusableOak import array, layout
from PrepareOakAssembly import load_part, save
from ReplayOakAssembly import BUILD, REPO, TREE, UE, Hierarchy, mesh, viewer_assets
from ReusableOakSprays import ROOT as PARTS, TRANSFORMS, expand_parts
from ScoreOakAssembly import FOCUS
from ScoreReusableOak import depth, projected, slots

OUT = BUILD / "oak-mixed-spray"
ENGINE = Path("C:/Program Files/Epic Games/UE_5.8")
GROUP_FILE = OUT / "group.json"
UE_INPUT = OUT / "assembly.json"
STEPS = 51


def select_group():
    # Rank every full batch before baking by first-visible pixels in the parent bough.
    groups = layout()[7]
    bounds = [1400, 740, 2180, 1500]
    depths = [projected(PARTS/"spray.mesh", g[1], 100, 132, bounds) for g in groups]
    for z in depths:
        if np.isfinite(z[[0,-1]]).any() or np.isfinite(z[:,[0,-1]]).any():
            raise ValueError("The exposure crop clips a spray")
    nearest = np.minimum.reduce(depths)
    ranks = []
    for g,z in zip(groups,depths):
        visible = np.isfinite(z) & (z==nearest)
        ranks.append(dict(first=g[0],placements=len(g[1]),visible=int(visible.sum()),
                          isolated=int(np.isfinite(z).sum())))
    group = max((r for r in ranks if r["placements"]==12), key=lambda r:r["visible"])
    ids = next(g[1] for g in groups if g[0]==group["first"])
    write_csv(OUT/"exposure.csv",ranks)
    group["ids"] = ids.tolist()
    GROUP_FILE.write_text(json.dumps(group,indent=2))
    root = []
    for slot in (0,1):
        v,idx = expand_parts(load_part(slot), ids[:1])
        root.append(dict(vertices=(v[:,[0,2,1]]*100).tolist(),normals=v[:,[3,5,4]].tolist(),
                         uv0=v[:,6:8].tolist(),uv1=v[:,8:10].tolist(),triangles=idx.tolist()))
    data = dict(root=root, others=[TRANSFORMS[i] for i in ids[1:]])
    asset_hash = hashlib.sha256(json.dumps(data).encode()).hexdigest()[:10]
    data["asset"] = "OakMixedSpray"+asset_hash
    UE_INPUT.write_text(json.dumps(data))
    print("Selected",group,flush=True)


def test_dir(yaw):
    folder=OUT if yaw==132 else OUT/f"yaw-{yaw}"
    folder.mkdir(exist_ok=True)
    return folder


def prefix(yaw):
    return "mixed-spray" if yaw==132 else f"mixed-spray-y{yaw}"


def unreal(script, label, marker, env, folder=OUT):
    log = folder/(label+".log")
    started = time.time_ns()
    run = subprocess.run([ENGINE/"Engine/Binaries/Win64/UnrealEditor-Cmd.exe",
        UE.parent/"FoliageCreation.uproject", "-EnablePlugins=PythonScriptPlugin,NaniteAssemblyEditorUtils,OakAssemblyBench",
        f"-ExecutePythonScript={REPO/'Tools'/script}",f"-abslog={log}","-unattended","-RenderOffscreen",
        "-NoSound","-NoSplash","-NoP4","-NoCrashDialog","-DDC=NoShared"],
        env={**os.environ,**env},creationflags=subprocess.CREATE_NO_WINDOW,stdout=subprocess.DEVNULL,timeout=1800)
    output = log.read_text(encoding="utf-8",errors="replace")
    if marker not in output or log.stat().st_mtime_ns < started:
        raise RuntimeError(f"Unreal did not finish {label}. See {log}")
    if run.returncode:
        after = output.split(marker)[-1]
        if run.returncode & 0xffffffff != 0xc0000005 or "Log file closed" not in after or "LogExit: Exiting." not in after:
            run.check_returncode()
        print(f"{label}: UE returned 0xc0000005 after logging completion and closing its log.",flush=True)


def native(yaw=132):
    folder=test_dir(yaw)
    asset = json.loads(UE_INPUT.read_text())["asset"]
    for voxels in (False, True):
        suffix = "Voxels" if voxels else ""
        path = UE.parent/"Content/NaniteOakBench"/(asset+suffix+".uasset")
        if not path.exists():
            unreal("MakeOakJointControl.py","build-native-"+(suffix or "Triangles"),"OAK_BENCH ASSEMBLIES_COMPLETE",
                   dict(OAK_SPRAY_GROUP_INPUT=str(UE_INPUT),OAK_VOXEL_CONTROL=str(int(voxels))))
        if not path.is_file():
            raise FileNotFoundError(path)
    jobs = []
    for name,suffix,mode,error in (("fine","","Mask",.001),("tri","","Mask",1),("vox","Voxels","Mask",1),
                                    ("tri-count","","OffscreenStats",1),("vox-count","Voxels","OffscreenStats",1)):
        jobs.append((name,suffix,mode,error))
    for repeat, order in enumerate((("tri","vox"),("vox","tri"))):
        for arm in order:
            jobs.append((f"{arm}-gpu-{repeat}","Voxels" if arm=="vox" else "","Profile",1))
    planned = [dict(asset=asset+suffix,name=prefix(yaw)+"-"+name,mode=mode,max_pixels_per_edge=error,
        resolution=[3840,2160],vertical_fov=45,isolated=True,capture_only=True,frustum=1,hzb=0,
        distance=50,yaw=yaw,pitch=-2.235,mesh_path="/Game/NaniteOakBench/"+asset+suffix,steps=STEPS)
        for name,suffix,mode,error in jobs]
    file=folder/"native-jobs.json"
    file.write_text(json.dumps(planned))
    started=time.time_ns()
    unreal("CaptureMixedOakMotion.py","native-motion","OAK_CAPTURE CAPTURES_COMPLETE",
           dict(OAK_CAPTURE_JOBS=str(file),OAK_MIXED_DIR=str(folder)),folder)
    for job in planned:
        name=job["name"]
        paths=([UE.parent/"Saved/Profiling/CSV"/(name+".csv")] if job["mode"]=="Profile" else
               [folder/"native"/f"{name}-{f:03}.png" for f in range(2*STEPS)])
        if any(p.stat().st_mtime_ns<started for p in paths):
            raise RuntimeError(f"Missing fresh native frames for {name}")


def leaf_components(v, faces):
    _,vertex=np.unique(v[:,:3].round(5),axis=0,return_inverse=True)
    vertex=vertex.ravel()
    parent=list(range(len(v)))
    def root(a):
        while a!=parent[a]:
            parent[a]=parent[parent[a]]
            a=parent[a]
        return a
    for a,b,c in vertex[faces]:
        parent[root(b)]=root(a)
        parent[root(c)]=root(a)
    return np.unique([root(a) for a in vertex[faces[:,0]]],return_inverse=True)[1].ravel()


def expanded(path,ids):
    v,t,labels=slots(path)
    verts,faces=expand_parts((v,t),ids)
    return verts,faces,np.tile(labels,len(ids))


def bake(protected_fraction=.86, brick_budget=28):
    group=json.loads(GROUP_FILE.read_text())
    ids=group["ids"]
    source=PARTS/"spray.mesh"
    coarse=PARTS/"mixed-part/100-56.mesh"
    v,t,labels=slots(source)
    leaf=t[labels==0]
    components=leaf_components(v,leaf)
    n=int(components.max()+1)
    if n!=45:
        raise ValueError(f"Expected 45 source leaves, got {n}")
    # Assign coarse faces to the nearest original leaf using their three corners and centroid.
    cv,ct,cl=slots(coarse)
    ct_leaf=ct[cl==0]
    coarse_xyz=cv[ct_leaf,:3]
    probes=np.concatenate([coarse_xyz,coarse_xyz.mean(1)[:,None,:]],axis=1)
    errors=[]
    for i in range(n):
        samples=v[leaf[components==i],:3]
        ref=np.concatenate([samples.reshape(-1,3),samples.mean(1)])
        errors.append(np.square(probes[:,:,None,:]-ref).sum(-1).min(-1).sum(1))
    coarse_component=np.argmin(errors,axis=0)
    full,faces,slots_full=expanded(source,ids)
    _,whole_wood=expand_parts((cv,ct[cl==1]),ids)
    coarse_v,coarse_faces=expand_parts((cv,ct_leaf),ids)
    group_components=(components+np.arange(len(ids))[:,None]*n).ravel()
    coarse_components=(coarse_component+np.arange(len(ids))[:,None]*n).ravel()
    leaves=faces[slots_full==0]
    training=[]
    value=np.zeros(n*len(ids))
    for yaw in (90,165,225):
        camera=view(yaw)
        xyz=(full[:,:3]-FOCUS)@camera.T
        xy=xyz[:,:2]*[2607.35065,-2607.35065]/(100-xyz[:,2:])+[1920,1080]
        crop=np.r_[np.floor(xy.min(0)-6),np.ceil(xy.max(0)+6)].astype(int)
        z=depth(full,faces,slots_full,100,yaw,crop)
        mask=np.isfinite(z)
        rim=outline(mask)
        leaf_z=depth(full,leaves,np.zeros(len(leaves)),100,yaw,crop)
        for i in range(len(value)):
            f=leaves[group_components==i]
            first=depth(full,f,np.zeros(len(f)),100,yaw,crop)
            hit=mask & (first==z)
            value[i]+=np.count_nonzero(hit)+4*np.count_nonzero(hit & rim)
        training.append((yaw,crop,z,mask & (z==leaf_z),rim))
    preserve=np.zeros(len(value),bool)
    preserve[np.argsort(-value,kind="stable")[:round(len(value)*protected_fraction)]]=True
    residual=coarse_faces[preserve[coarse_components]]
    other=leaves[~preserve[group_components]]
    save(OUT/"residual.mesh",coarse_v,residual)
    save(OUT/"interior.mesh",full,other)
    (OUT/"interior.mat").write_bytes(bytes(len(other)))
    edge=.029
    sampled=OUT/"interior.cells"
    subprocess.run([EXE,"sample",OUT/"interior.mesh","0",str(edge),sampled],check=True)
    cells=np.fromfile(sampled,dtype=[("p","<i4",3),("cov","<f4")])
    cells=cells[cells["cov"]>0]
    p=cells["p"]
    centers=(p+.5)*edge
    blocks,inv=np.unique(p//4,axis=0,return_inverse=True)
    inv=inv.ravel()
    positive=np.zeros(len(p))
    penalty=np.zeros(len(p))
    weights=[]
    for yaw,crop,z,leaf_first,rim in training:
        remain=np.vstack([residual,whole_wood])
        rz=depth(coarse_v,remain,np.r_[np.zeros(len(residual)),np.ones(len(whole_wood))],100,yaw,crop)
        pair=np.concatenate(list(rays(p,edge,np.arange(len(p)),yaw,crop)))
        cell,pix=pair.T
        zcell=100-(centers-FOCUS)@view(yaw)[2]
        expected=z.ravel()[pix]
        correct=leaf_first.ravel()[pix] & (np.abs(zcell[cell]-expected)<2*edge)
        need=rz.ravel()[pix] > zcell[cell]+edge
        hit=correct * (1+2*rim.ravel()[pix]) * np.where(need,1.,.1)
        bad=(~np.isfinite(expected))*1.4 + ((zcell[cell]<expected-2*edge)&np.isfinite(expected))*.7
        positive+=np.bincount(cell,weights=hit,minlength=len(p))
        penalty+=np.bincount(cell,weights=bad,minlength=len(p))
        weights.append((cell,pix,hit,bad))
    viable=(positive>penalty) & (positive>0)
    # Take a brick's best remaining first hit per camera pixel, then discount pixels it already covers.
    observations=[]
    for cell,pix,hit,bad in weights:
        keys=inv[cell[viable[cell]]].astype(np.int64)*1000000+pix[viable[cell]]
        uniq,back=np.unique(keys,return_inverse=True)
        gain=np.zeros(len(uniq));loss=np.zeros(len(uniq))
        np.maximum.at(gain,back.ravel(),hit[viable[cell]])
        np.maximum.at(loss,back.ravel(),bad[viable[cell]])
        observations.append((uniq//1000000,uniq%1000000,gain,loss))
    chosen=np.zeros(len(blocks),bool)
    for _ in range(brick_budget):
        benefit=sum(np.bincount(b,weights=g-l,minlength=len(blocks)) for b,_,g,l in observations)
        benefit[chosen]=-np.inf
        index=int(np.argmax(benefit))
        if benefit[index]<=0:break
        chosen[index]=True
        for b,pix,gain,_ in observations:
            gain[np.isin(pix,pix[(b==index)&(gain>0)])]=0
    active=np.flatnonzero(viable & chosen[inv])
    bit=(p%4)@np.array([1,4,16])
    active=active[np.argsort(inv[active]*64+bit[active])]
    # Pull shaded normals from the residual-complement source surfaces only.
    xyz=full[other,:3].mean(1)
    ns=full[other,3:6].mean(1)
    bricks=[]
    for ix in np.split(active,np.flatnonzero(np.diff(inv[active]))+1):
        near=np.square(centers[ix,None,:]-xyz[None,:,:]).sum(-1).argmin(1)
        normals=ns[near].copy()
        normals/=np.maximum(np.linalg.norm(normals,axis=1,keepdims=True),1e-9)
        frame=np.vstack([blocks[inv[ix[0]]]*4*edge,np.eye(3)*edge])
        bricks.append((frame,sum(1<<int(b) for b in bit[ix]),normals))
    if not bricks or not len(residual):raise ValueError("Missing mixed representation")
    group_layout=next(g for g in layout()[7] if g[0]==group["first"])
    write_bake(OUT/"mixed.oakvx",[bricks,[]],3,[group_layout],mixed=True)
    (OUT/"bake.json").write_text(json.dumps(dict(cell_m=edge,training_yaws=[90,165,225],total_leaves=len(value),
        protected=int(preserve.sum()),residual_leaf_triangles=len(residual),bricks=len(bricks),cells=len(active)),indent=2))


def flatten(tree):
    nodes=[None]; tags=[0]
    def emit(item,at):
        raw,kind,children=item
        first=len(nodes)
        nodes[at]=raw[:40]+struct.pack("<II",first if children else 0,len(children))+raw[48:]
        tags[at]=kind
        nodes.extend([None]*len(children));tags.extend([0]*len(children))
        for j,child in enumerate(children):emit(child,first+j)
    emit(tree,0)
    return b"".join(nodes),tags if any(tags) else []


def stage(arm, mixed_error=.021, crown=False):
    source=PARTS/"pilot-7"
    dest=viewer_assets(str(OUT/(("crown-" if crown else "")+arm)),source)
    h=Hierarchy((source/"assets"/TREE).read_bytes())
    group=json.loads(GROUP_FILE.read_text())
    start,ids,box=next(g for g in layout()[7] if g[0]==group["first"])
    count=len(ids)
    policy=json.loads((PARTS/"policy.json").read_text())
    flags=[]
    for slot,part in enumerate(h.parts):
        original=part[1][:56]
        total=struct.unpack_from("<I",original,52)[0]
        rootbox=original[:24]
        part[2]=bytes(40)
        part[3]=bytes(12)
        def node(first,size,geometry=(0,0),error=0,kind=0,children=(),bounds=box):
            return (bytes(bounds)+struct.pack("<IIff4I",*geometry,error,error,0,0,first,size),kind,children)
        def append(v,f):
            begin=len(part[3])//4
            used,idx=np.unique(f,return_inverse=True)
            part[3]+=(idx.ravel()+len(part[2])//40).astype("<u4").tobytes()
            part[2]+=v[used].astype("<f4").tobytes()
            return begin,f.size
        if slot==2:
            root=node(0,total,(0,3),bounds=rootbox)
        else:
            branch=None
            for limit,path in reversed(policy if arm!="fine" else [(0,"spray.mesh")]):
                v,t,s=slots(PARTS/path)
                geometry=append(v,t[s==slot])
                branch=node(start,count,geometry,1/limit if branch else 0,1,[branch] if branch else [])
            if arm in ("mixed","coarse"):
                if slot==0 and arm=="mixed":
                    residual=node(start,count,append(*mesh(OUT/"residual.mesh")))
                else:
                    v,t,s=slots(PARTS/"mixed-part/100-56.mesh")
                    residual=node(start,count,append(v,t[s==slot]),kind=1)
                branch=node(start,count,error=mixed_error,kind=3 if arm=="mixed" else 2,children=[residual,branch])
            root=node(0,total,children=[node(0,start,(0,3)),branch,
                      node(start+count,total-start-count,(0,3))],bounds=rootbox)
        part[1],tags=flatten(root)
        flags.append(tags)
    if crown:
        full=Hierarchy((PARTS/"pilot"/"assets"/TREE).read_bytes())
        if arm in ("tri","mixed","coarse"):
            if arm!="tri":
                at=0; original_arrays=[]
                for size in [4]*len(full.parts)+[1]*len(full.parts):
                    n,=struct.unpack_from("<I",full.tail,at)
                    original_arrays.append(full.tail[at+4:at+4+n*size]);at+=4+n*size
                if at!=len(full.tail):raise ValueError("Unrecognized tree tail")
                for slot in (0,1):
                    part=full.parts[slot]; raw=part[1]
                    kinds=original_arrays[len(full.parts)+slot]
                    index=next(i for i,kind in enumerate(kinds) if kind==1 and
                               struct.unpack_from("<II",raw,56*i+48)==(start,count))
                    # Reuse precisely the mixed geometry from the isolated fixture and the original fallback in situ.
                    test=h.parts[slot]
                    route_kind=3 if arm=="mixed" else 2
                    route=flags[slot].index(route_kind)
                    child=struct.unpack_from("<I",test[1],56*route+40)[0]
                    n=bytearray(test[1][child*56:(child+1)*56])
                    begin,num=struct.unpack_from("<II",n,24)
                    indices=np.frombuffer(test[3],"<u4",num,4*begin)
                    verts=np.frombuffer(test[2],"<f4").reshape(-1,10)
                    used,remap=np.unique(indices,return_inverse=True)
                    struct.pack_into("<II",n,24,len(part[3])//4,num)
                    part[3]+=(remap.ravel()+len(part[2])//40).astype("<u4").tobytes()
                    part[2]+=verts[used].tobytes()
                    def rebuild(i):
                        node_bytes=raw[i*56:(i+1)*56]
                        first,num_children=struct.unpack_from("<II",node_bytes,40)
                        tree=(node_bytes,kinds[i],[rebuild(k) for k in range(first,first+num_children)])
                        if i==index:
                            return test[1][56*route:56*(route+1)],route_kind,[(n,flags[slot][child],[]),tree]
                        return tree
                    part[1],new_kinds=flatten(rebuild(0))
                    original_arrays[len(full.parts)+slot]=bytes(new_kinds)
                full.tail=b"".join(struct.pack("<I",len(a)//size)+a for a,size in
                    zip(original_arrays,[4]*len(full.parts)+[1]*len(full.parts)))
            full.write(dest/"assets"/TREE)
            return dest/"CustomGameEngine.exe"
        raise ValueError("The crown timing control supports triangle and mixed cuts.")
    h.tail=b"".join(array([],"<u4") for _ in h.parts)+b"".join(array(f,"u1") for f in flags)
    with tempfile.NamedTemporaryFile(dir=dest/"assets",delete=False) as f:pending=Path(f.name)
    try:
        h.write(pending);pending.replace(dest/"assets"/TREE)
    finally:
        pending.unlink(missing_ok=True)
    return dest/"CustomGameEngine.exe"


def flags(arm,distance,yaw=132):
    args=["--scene","forked-oak","--camera","overview","--width","3840","--height","2160","--msaa","1",
        "--distance",str(distance),"--yaw-degrees",str(yaw),"--pitch-degrees","-2.235","--wind-strength","0",
        "--lod","auto","--spray-mode","hierarchy","--tree-submit","direct","--spray-culling","on",
        "--spray-local-lod","on","--spray-pixel-error","1","--cull-spray-wood-backfaces"]
    if arm=="mixed":args+=["--bough-voxel-bake",str(OUT/"mixed.oakvx")]
    return args


def d3d(timing=False,yaw=132,crown=False,mask_arms=("fine","tri","mixed","coarse")):
    folder=test_dir(yaw)
    if crown:
        folder=folder/"crown"
        folder.mkdir(exist_ok=True)
    views={arm:stage(arm,crown=crown) for arm in (("tri","mixed","coarse") if timing else mask_arms)}
    orders=(("tri","mixed","coarse"),("coarse","mixed","tri"),("mixed","tri","coarse"),("coarse","tri","mixed")) if timing else (mask_arms,)
    for repeat,order in enumerate(orders):
        for arm in order:
            for leg,(start,end) in enumerate(((50,100),(100,50))):
                name=f"{'cost' if timing else 'mask'}-{arm}-{repeat}-{leg}"
                args=flags(arm,start,yaw)+["--frames",str(STEPS),"--end-distance",str(end),"--fps","30"]
                if crown:args.remove("--cull-spray-wood-backfaces")
                args+=["--sequence-timings-only"] if timing else ["--geometry-mask","--probe-gi","off"]
                with tempfile.TemporaryDirectory(dir=folder) as tmp:
                    path=Path(tmp)/"frames"
                    with (folder/(name+".log")).open("w") as f:
                        subprocess.run([views[arm],"--capture",path,*args],stdout=f,stderr=f,timeout=180,check=True)
                    final=folder/name
                    final.mkdir(exist_ok=True)
                    with (path/"frames.csv").open() as f:
                        rows=list(csv.DictReader(f))
                    if len(rows)!=STEPS or abs(float(rows[-1]["distance"])-end)>.01:
                        raise ValueError(f"Incomplete camera path at {name}")
                    if not timing and len(list(path.glob("frame-*.png")))!=STEPS:
                        raise ValueError(f"Missing masks at {name}")
                    for p in path.iterdir():p.replace(final/p.name)
            print("Captured",name,flush=True)


def csv_rows(path):
    with path.open() as f:return list(csv.DictReader(f))


def mask_at(arm,leg,k,folder,yaw):
    if arm.startswith("ue-"):
        return folder/"native"/f"{prefix(yaw)}-{arm[3:]}-{leg*STEPS+k:03}.png"
    return folder/f"mask-{arm}-0-{leg}"/f"frame-{k:06}.png"


def small_mask(path,native,box):
    with Image.open(path) as im:
        if im.size!=(3840,2160):raise ValueError(f"Unexpected camera size in {path}")
        rgb=np.asarray(im.crop(box).convert("RGB"))
    return ((rgb[:,:,1]>rgb[:,:,0])&(rgb[:,:,1]>rgb[:,:,2])) if native else rgb.any(axis=2)


def quality(yaw=132):
    from BenchmarkOakGpuTraversal import native_counter
    from PIL import ImageDraw,ImageFont
    # A single fixed crop contains the entire dolly; use the finest masks to locate it.
    folder=test_dir(yaw)
    bounds=[]
    for arm in ("fine","ue-fine"):
        for k in (0,STEPS//2,STEPS-1):
            y,x=np.where(read_mask(mask_at(arm,0,k,folder,yaw),arm.startswith("ue-")))
            bounds.append((x.min(),y.min(),x.max()+1,y.max()+1))
    values=np.array(bounds)
    box=tuple(np.r_[values[:,:2].min(0)-20,values[:,2:].max(0)+20])
    widths,heights=(values[:,2:]-values[:,:2]).max(0)+8
    scale=min(224/widths,172/heights)
    half_width,half_height=int(np.ceil(112/scale)),int(np.ceil(86/scale))
    rows=[]; frames=[]
    font=ImageFont.truetype("segoeui.ttf",16)
    for leg in (0,1):
        previous=None
        work={arm:csv_rows(folder/f"mask-{arm}-0-{leg}/frames.csv") for arm in ("tri","mixed","coarse")}
        for k in range(STEPS):
            masks={arm:small_mask(mask_at(arm,leg,k,folder,yaw),arm.startswith("ue-"),box)
                   for arm in ("fine","tri","mixed","coarse","ue-fine","ue-tri","ue-vox")}
            if any(m[[0,-1]].any() or m[:,[0,-1]].any() or not m.any() for m in masks.values()):
                raise ValueError("An empty mask or a clipped moving spray")
            d=50+(k if leg==0 else STEPS-1-k)
            scores={}
            for arm in ("ue-vox","ue-tri","tri","mixed","coarse"):
                ref_name="ue-fine" if arm.startswith("ue-") else "fine"
                ref=masks[ref_name]; m=masks[arm]
                _,_,iou,_,_,edge=next(measure(ref,[(0,0,m)]))
                temporal=None
                if previous:
                    diff=(m^previous[arm])^(ref^previous[ref_name])
                    temporal=diff.sum()/(m|ref|previous[arm]|previous[ref_name]).sum()
                if arm.startswith("ue-"):
                    stat=folder/"native"/f"{prefix(yaw)}-{arm[3:]}-count-{leg*STEPS+k:03}.png"
                    tris,bricks=native_counter(stat,424),native_counter(stat,456)
                    submitted=None
                else:
                    c=work[arm][k]
                    # Two off-group sentinel draws per spray material and one trunk sentinel.
                    submitted=sum(int(c[key]) for key in ("camera_leaf_triangles","camera_spray_wood_triangles","camera_tree_trunk_triangles"))-5
                    bricks=int(c["camera_voxel_bricks"]);tris=submitted-12*bricks
                scores[arm]=dict(leg=leg,frame=k,distance=d,arm=arm,surface_triangles=tris,bricks=bricks,
                    submitted=submitted,iou=iou,outline=edge,temporal=temporal)
                common=next(measure(masks["ue-fine"],[(0,0,m)]))
                scores[arm].update(common_iou=common[2],common_outline=common[-1])
            target=scores["ue-vox"]
            agreement=np.count_nonzero(masks["fine"]&masks["ue-fine"])/np.count_nonzero(masks["fine"]|masks["ue-fine"])
            for row in scores.values():
                row["passed"]=row["iou"]>=target["iou"] and row["outline"]<=target["outline"]
                row["common_passed"]=row["common_iou"]>=target["iou"] and row["common_outline"]<=target["outline"]
                row["temporal_passed"]=(None if row["temporal"] is None else row["temporal"]<=target["temporal"])
                row["fine_agreement"]=agreement
            rows.extend(scores.values())
            previous=masks
            if leg==0 or 0<k<STEPS-1:
                img=Image.new("RGB",(1140,237),"#1b2326")
                draw=ImageDraw.Draw(img)
                draw.text((9,3),f"{d} m   {'outward' if not leg else 'return'}     White = shared tree pixels     Blue = missing     Orange = extra",font=font,fill="white")
                y,x=np.where(masks["fine"]);cx=(x.max()+x.min())//2;cy=(y.min()+y.max())//2
                for col,(arm,label) in enumerate((("fine","Finest triangles"),("tri","Triangle baseline"),("mixed","Mixed cut"),("coarse","Coarser triangles"),("ue-vox","Nanite default"))):
                    ref=masks["ue-fine" if arm=="ue-vox" else "fine"];m=masks[arm]
                    rgb=np.full((*m.shape,3),[33,43,45],np.uint8)
                    rgb[ref&m]=[213,222,214];rgb[ref&~m]=[72,186,224];rgb[m&~ref]=[250,152,71]
                    crop=Image.fromarray(rgb).crop((cx-half_width,cy-half_height,cx+half_width,cy+half_height)).resize((224,172),Image.Resampling.NEAREST)
                    img.paste(crop,(2+col*228,58));draw.text((6+col*228,32),label,fill="white",font=font)
                if leg==0 and k==STEPS-1:
                    img.save(folder/"comparison-100m.png")
                frames.append(img)
    write_csv(folder/"quality.csv",rows)
    frames[0].save(folder/"motion.gif",save_all=True,append_images=frames[1:],
                   duration=[30 if i%3!=2 else 40 for i in range(len(frames))],loop=0,disposal=2)


def costs(yaw=132):
    folder=test_dir(yaw)
    rows=[]
    for repeat in range(4):
        for arm in ("tri","mixed","coarse"):
            for leg in (0,1):
                samples=csv_rows(folder/f"cost-{arm}-{repeat}-{leg}/frames.csv")
                for r in samples:
                    if min(float(r[k]) for k in ("gpu_asset_ms","gpu_camera_ms","gpu_frame_ms"))<=0:
                        raise ValueError("Missing moving GPU timestamps")
                    brick_ms=float(r["gpu_bough_brick_ms"])
                    if (int(r["camera_voxel_bricks"])>0)!=(brick_ms>0):
                        raise ValueError("Bough brick work and GPU timestamps disagree")
                    rows.append(dict(arm=arm,repeat=repeat,leg=leg,frame=r["frame"],distance=r["distance"],
                        tree_ms=float(r["gpu_asset_ms"])+brick_ms,brick_ms=brick_ms,
                        asset_ms=r["gpu_asset_ms"],camera_ms=r["gpu_camera_ms"],frame_ms=r["gpu_frame_ms"]))
    write_csv(folder/"d3d-gpu.csv",rows)
    rows=[]
    for repeat in (0,1):
        for arm in ("tri","vox"):
            path=UE.parent/"Saved/Profiling/CSV"/f"{prefix(yaw)}-{arm}-gpu-{repeat}.csv"
            samples=[r for r in csv_rows(path) if (r.get("FrameTime") or "").replace(".","",1).isdigit()]
            if len(samples)!=2*STEPS+5:raise ValueError(f"Unexpected moving profile length in {path}")
            samples=samples[3:3+2*STEPS]
            totals=[float(r["GPU/NaniteVisBuffer"])+float(r["GPU/NaniteBasePass"]) for r in samples]
            rows.append(dict(arm=arm,repeat=repeat,samples=len(samples),visibility_base_ms=median(totals)))
    write_csv(folder/"native-gpu.csv",rows)


def crown_cost():
    d3d(timing=True,crown=True)
    folder=OUT/"crown"
    rows=[]
    for repeat in range(4):
        for leg in (0,1):
            baseline=csv_rows(folder/f"cost-tri-{repeat}-{leg}/frames.csv")
            candidate=csv_rows(folder/f"cost-mixed-{repeat}-{leg}/frames.csv")
            coarse=csv_rows(folder/f"cost-coarse-{repeat}-{leg}/frames.csv")
            if not any(int(r["camera_voxel_bricks"]) for r in candidate):
                raise ValueError("The mixed group was never visited in the crown")
            for a,b,c in zip(baseline,candidate,coarse,strict=True):
                if any(a[k]!=r[k] for r in (b,c) for k in ("distance","shadow_triangles")):
                    raise ValueError("Crown cameras or shadow cuts disagree")
                for arm,r in (("tri",a),("mixed",b),("coarse",c)):
                    rows.append(dict(arm=arm,repeat=repeat,leg=leg,distance=r["distance"],
                        bricks=r["camera_voxel_bricks"],submitted=sum(int(r[k]) for k in
                        ("camera_leaf_triangles","camera_spray_wood_triangles","camera_tree_trunk_triangles")),
                        tree_ms=float(r["gpu_asset_ms"])+float(r["gpu_bough_brick_ms"]),brick_ms=r["gpu_bough_brick_ms"],
                        asset_ms=r["gpu_asset_ms"],camera_ms=r["gpu_camera_ms"],frame_ms=r["gpu_frame_ms"]))
    write_csv(folder/"cost.csv",rows)


def summarize(yaw):
    folder=test_dir(yaw)
    data=csv_rows(folder/"quality.csv");gpu=csv_rows(folder/"d3d-gpu.csv")
    result={}
    for arm in ("tri","mixed","coarse","ue-tri","ue-vox"):
        frames=[r for r in data if r["arm"]==arm]
        motion=[r for r in frames if r["temporal"]]
        first=next((r for r in frames if int(r["bricks"])),None)
        result[arm]=dict(frames=len(frames),own_pass=sum(r["passed"]=="True" for r in frames),
            common_pass=sum(r["common_passed"]=="True" for r in frames),
            motion_pass=sum(r["temporal_passed"]=="True" for r in motion),transitions=len(motion),
            first_brick_m=int(first["distance"]) if first else None,
            motion_p95=float(np.percentile([float(r["temporal"]) for r in motion],95)),
            min_fine_agreement=min(float(r["fine_agreement"]) for r in frames))
        if arm in ("tri","mixed","coarse"):
            for term in ("tree_ms","camera_ms","frame_ms"):
                repeats=[median(float(r[term]) for r in gpu if r["repeat"]==str(i) and r["arm"]==arm) for i in range(4)]
                result[arm][term]=median(repeats)
                result[arm][term+"_repeats"]=repeats
        else:
            result[arm]["visibility_base_ms"]=median(float(r["visibility_base_ms"]) for r in
                csv_rows(folder/"native-gpu.csv") if r["arm"]==arm[3:])
    (folder/"summary.json").write_text(json.dumps(result,indent=2))


if __name__=="__main__":
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument("action",choices=("stage","view","benchmark","score"))
    parser.add_argument("--arm",choices=("mixed","tri","coarse"),default="mixed")
    parser.add_argument("--distance",type=int,choices=range(50,101),default=100,metavar="50..100")
    parser.add_argument("--crown",action="store_true",help="Open the group in the full oak")
    parser.add_argument("--unreal",type=Path,default=ENGINE)
    args=parser.parse_args()
    ENGINE=args.unreal
    OUT.mkdir(exist_ok=True)
    if args.action in ("stage","view","benchmark"):
        select_group()
        bake()
    if args.action in ("view","stage"):
        exe=stage(args.arm,crown=args.crown)
        args_view=flags(args.arm,args.distance)
        if args.crown:args_view.remove("--cull-spray-wood-backfaces")
        cmd=[str(exe),"--review-capture",*args_view]
        print(subprocess.list2cmdline(cmd))
        if args.action=="view":subprocess.Popen(cmd)
    else:
        for yaw in (132,23):
            if args.action=="benchmark":
                native(yaw);d3d(yaw=yaw);d3d(True,yaw)
            costs(yaw);quality(yaw);summarize(yaw)
        if args.action=="benchmark":crown_cost()
