"""Compare material-preserving group reduction with independently reduced leaf and wood cuts."""
from pathlib import Path
import csv
import numpy as np
from ReplayOakAssembly import OUT,UE,REPO,PREVIOUS,TREE,Hierarchy,capture,mesh,measure,quality,read_mask,stage,write
from ScoreOakAssembly import rasterize,FOCUS

def front(v,ix,d,yaw):
    y,p=np.radians([yaw,-5.46 if d==41 else -2.235])
    eye=FOCUS+d*np.array([np.sin(y)*np.cos(p),np.sin(p),np.cos(y)*np.cos(p)])
    a,b,c=v[ix,:3].transpose(1,0,2)
    return ((np.cross(b-a,c-a)*(eye-a)).sum(1)>=0)

def mixed_raster(file,d,yaw,wood=False):
    v,i=mesh(file)
    if wood:i=i[front(v,i,d,yaw)]
    elif file.with_suffix(".mat").exists():
        slots=np.fromfile(file.with_suffix(".mat"),np.uint8)
        assert len(slots)==len(i) and set(slots)=={0,1}
        i=i[(slots==0)|front(v,i,d,yaw)]
    return rasterize((v,i),d,yaw)

def run():
    rows=[]
    leaves=sorted(p for p in OUT.glob("leaf-*-mixedtest/level*.mesh") if "paired" not in p.name and int(p.stem[-2:])<=7)
    wood=[OUT/"epic-0.5/level02.mesh",OUT/"epic-0.5/level04-paired.mesh",OUT/"custom-0.68/level06.mesh"]
    joint=sorted(p for p in OUT.glob("joint-*-mixedtest/level*.mesh") if int(p.stem[5:7])<=7)
    sizes={p:len(mesh(p)[1]) for p in leaves+wood+joint}
    for d in (41,100):
        for yaw in (132,90,180):
            suffix="" if yaw==132 else f"-yaw{yaw}"
            ti,te=quality(UE/f"joint-cut-{d}{suffix}.png",read_mask(UE/f"joint-cut-{d}{suffix}-fine.png",True),True)
            rz=np.minimum(mixed_raster(OUT/"leaf.mesh",d,yaw),mixed_raster(OUT/"wood.mesh",d,yaw,True))
            ys,xs=np.where(np.isfinite(rz))
            crop=(slice(max(0,ys.min()-32),min(2160,ys.max()+33)),slice(max(0,xs.min()-32),min(3840,xs.max()+33)))
            rz=rz[crop].copy();ref=np.isfinite(rz)
            def record(kind,a,b,count,z):
                occupied=np.isfinite(z)
                m=next(measure(ref,[(1,count,occupied)]))
                shared=ref&occupied
                rows.append(dict(d=d,yaw=yaw,method=kind,leaf_or_joint=a.relative_to(OUT).as_posix(),
                                 wood=b.relative_to(OUT).as_posix() if b else "",triangles=count,iou=m[2],edge=m[-1],
                                 ue_iou=ti,ue_edge=te,passed=m[2]>=ti and m[-1]<=te,
                                 p95_mm=np.percentile(abs(z[shared]-rz[shared])*1000,95)))
            woods={b:mixed_raster(b,d,yaw,True)[crop].copy() for b in wood}
            for a in leaves:
                z=mixed_raster(a,d,yaw)[crop]
                for b,wz in woods.items(): record("separate",a,b,sizes[a]+sizes[b],np.minimum(z,wz))
            for a in joint:record("joint",a,None,sizes[a],mixed_raster(a,d,yaw)[crop])
        write("joint-sweep.csv",rows)

def stage_joint(name,parts):
    dest=stage(name,OUT/"wood.mesh")
    h=Hierarchy((dest/"assets"/TREE).read_bytes())
    for part,(v,ii) in zip(h.parts,parts):
        used,remap=np.unique(ii,return_inverse=True)
        part[2]=v[used].astype("<f4").tobytes();part[3]=remap.astype("<u4").tobytes()
        import struct
        struct.pack_into("<I",part[1],28,ii.size)
    h.write(dest/"assets"/TREE)
    return dest

def finalist(path,wood=None):
    if wood:return [mesh(path),mesh(wood)]
    v,i=mesh(path);slots=np.fromfile(path.with_suffix(".mat"),np.uint8)
    return [(v,i[slots==s]) for s in (0,1)]

if __name__=="__main__":run()
