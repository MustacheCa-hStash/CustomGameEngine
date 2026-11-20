"""Fit occupied oak cells at 4K with the project's binary fitter and a coverage-transfer control."""
import argparse
from functools import lru_cache
from pathlib import Path
import itertools
import hashlib
import struct
import subprocess

import numpy as np
from DecodeOakBoughVoxels import BITS, ROOT, write_bake
from PrepareOakAssembly import save
from ReplayOakAssembly import BUILD, OUT, mesh
from ScoreOakAssembly import FOCUS
from ScoreReusableOak import depth
from ReusableOakSprays import TRANSFORMS
from PackageReusableOak import layout

EXE=BUILD/"Release/BakeOakBoughVoxels.exe"
VIEWS=(132,90,180,45,225,0,270)

@lru_cache(None)
def digest(path):
    with path.open("rb") as f:return hashlib.file_digest(f,"sha256").hexdigest()

def current(cache,inputs):
    mark=cache.with_suffix(cache.suffix+".sha256")
    token=" ".join(digest(f) for f in inputs)
    return cache.exists() and mark.exists() and mark.read_text()==token,mark,token
def key(c):
    x,y,z=c.astype(np.int64).T
    return ((x+100000)<<38)+((y+100000)<<19)+(z+100000)

@lru_cache(2)
def geometry(group):
    path=OUT/("joint.mesh" if group=="merged" else "reusable/spray.mesh")
    v,i=mesh(path)
    return v,i,np.fromfile(path.with_suffix(".mat"),"u1")

def normals(coords,edge,group,slot):
    # Stable representative normal of the closest triangle sample in an intersecting cell.
    # The two occupancy arms share these attributes.
    v,tris,labels=geometry(group);ix=tris[labels==slot]
    center=v[ix,:3].mean(1); normal=v[ix,3:6].mean(1)
    xyz=np.concatenate((center,v[ix[:,0],:3],v[ix[:,1],:3],v[ix[:,2],:3]))
    grid=np.floor(xyz/edge).astype(int)
    order=np.lexsort((np.square(xyz/edge-grid-.5).sum(1),key(grid)))
    index=key(grid)[order];n=np.zeros((len(coords),3));missing=np.arange(len(coords))
    for delta in sorted(itertools.product((-1,0,1),repeat=3), key=lambda a:sum(x*x for x in a)):
        if not len(missing):break
        search=key(coords[missing]+delta);j=np.searchsorted(index,search).clip(0,len(index)-1)
        match=index[j]==search;n[missing[match]]=normal[order[j[match]]%len(ix)]
        missing=missing[~match]
    n[missing]=[0,1,0]
    n/=np.maximum(np.linalg.norm(n,axis=1,keepdims=True),1e-8)
    return n

def view(yaw):
    y,p=np.radians([yaw,-2.235]);back=np.array([np.sin(y)*np.cos(p),np.sin(p),np.cos(y)*np.cos(p)])
    right=np.array([np.cos(y),0,-np.sin(y)])
    return np.stack([right,np.cross(back,right),back])

def training(yaw,slot):
    path=ROOT/f"train-{yaw}-{slot}.npz"
    good,stamp,token=current(path,[OUT/"joint.mesh",OUT/"joint.mat",Path(__file__),Path(__file__).with_name("ScoreReusableOak.py")])
    if not good:
        v,i,l=geometry("merged");i=i[l==slot];l=np.full(len(i),slot)
        q=(v[:,:3]-FOCUS)@view(yaw).T
        xy=q[:,:2]*(2607.35065/(100-q[:,2:]))*[1,-1]+[1920,1080]
        crop=[*np.floor(xy.min(0)-5).astype(int),*np.ceil(xy.max(0)+5).astype(int)]
        ref=np.isfinite(depth(v,i,l,100,yaw,crop))
        np.savez(path,ref=ref,crop=crop)
        stamp.write_text(token)
    with np.load(path) as a:return a["ref"],a["crop"]

def rays(coords,edge,ids,yaw,crop):
    axes=view(yaw);eye=FOCUS+100*axes[2];width=crop[2]-crop[0]
    for begin in range(0,len(coords),16000):
        c=coords[begin:begin+16000];lo=c*edge;hi=lo+edge
        xyz=(lo[:,None,:]+np.array(list(itertools.product((0,edge),repeat=3)))-FOCUS)@axes.T
        xy=xyz[:,:,:2]*(2607.35065/(100-xyz[:,:,2:]))*[1,-1]+[1920,1080]-crop[:2]
        a=np.maximum(np.ceil(xy.min(1)-.5).astype(int),0);b=np.minimum(np.floor(xy.max(1)-.5).astype(int),crop[2:]-crop[:2]-1)
        size=np.maximum(b-a+1,0);n=size.prod(1);cell=np.repeat(np.arange(len(n)),n)
        off=np.arange(len(cell))-np.repeat(np.cumsum(n)-n,n)
        x=a[cell,0]+off%size[cell,0];y=a[cell,1]+off//size[cell,0]
        direction=(x[:,None]+crop[0]-1919.5)*axes[0]/2607.35065-(y[:,None]+crop[1]-1079.5)*axes[1]/2607.35065-axes[2]
        t0=(lo[cell]-eye)/direction;t1=(hi[cell]-eye)/direction
        hit=np.minimum(t0,t1).max(1)<np.maximum(t0,t1).min(1)
        yield np.column_stack((ids[begin+cell[hit]],(y*width+x)[hit])).astype("<u4")

def prepare(group,mm,slot,budget):
    edge=mm/1000
    stem=ROOT/f"{group}-{mm}-{slot}"; file=stem.with_suffix(".cells")
    path=OUT/("joint.mesh" if group=="merged" else "reusable/spray.mesh")
    good,stamp,token=current(file,[path,path.with_suffix(".mat"),EXE])
    if not good:
        subprocess.run([EXE,"sample",path,str(slot),str(edge),file],check=True)
        stamp.write_text(token)
    raw=np.fromfile(file,dtype=[("p","<i4",3),("c","<f4")]);raw=raw[raw["c"]>0]
    p=raw["p"];n=normals(p,edge,group,slot)
    source=stem.with_suffix(".samples")
    arr=np.empty(len(p),dtype=[("p","<i4",3),("cov","<f4"),("n","<f4",3)])
    arr["p"],arr["cov"],arr["n"]=p,raw["c"],n;arr.tofile(source)
    red=stem.with_suffix(".transfer")
    subprocess.run([EXE,"redistribute",source,red],check=True)
    c=np.fromfile(red,"<f4"); assert len(c)==len(p)
    blocks,inv=np.unique(p//4,axis=0,return_inverse=True);inv=inv.reshape(-1)
    bits=(p%4)@np.array([1,4,16]); bit=np.left_shift(np.uint64(1),bits.astype(np.uint64))
    count=np.bincount(inv,weights=np.where(c>0,c,0),minlength=len(blocks))
    rank=np.argsort(-count,kind="stable");chosen=rank[:min(budget,(count>0).sum())]
    allowed=np.zeros(len(blocks),bool);allowed[chosen]=True
    def pack(active):
        masks=np.zeros(len(blocks),np.uint64);np.bitwise_or.at(masks,inv[active],bit[active]);return masks
    seed=pack((c>0)&allowed[inv]);eligible=pack(allowed[inv])
    masks={"transfer":seed,"free":pack(c>0)}
    if group=="merged":
        training_path=stem.with_suffix(".fit")
        ids=np.cumsum(allowed)-1; active=allowed[inv]
        with training_path.open("wb") as f:
            f.write(struct.pack("<3I",allowed.sum(),len(chosen),len(VIEWS)))
            for block,avail,start in zip(blocks[allowed],eligible[allowed],seed[allowed]):
                f.write(struct.pack("<3iQQ",*block,avail,start))
            for yaw in VIEWS:
                target,crop=training(yaw,slot)
                pairs=np.concatenate(list(rays(p[active],edge,ids[inv[active]]*64+bits[active],yaw,crop)))
                f.write(struct.pack("<II",target.size,len(pairs)));f.write(target.astype("<u2").tobytes());f.write(pairs.tobytes())
        opt=stem.with_suffix(".optimized");subprocess.run([EXE,"fit",training_path,opt],check=True)
        out=np.fromfile(opt,dtype=[("p","<i4",3),("mask","<u8")]);fitted=np.zeros(len(blocks),np.uint64)
        fitted[np.searchsorted(key(blocks),key(out["p"]))]=out["mask"];masks["fit"]=fitted
    order=np.argsort(inv*64+bits)
    def bricks(mask):
        active=order[(mask[inv[order]]&bit[order])!=0];starts=np.r_[0,np.flatnonzero(np.diff(inv[active]))+1,len(active)]
        return [(np.vstack((blocks[inv[active[i]]]*4*edge,np.eye(3)*edge)),int(mask[inv[active[i]]]),n[active[i:j]])
                for i,j in zip(starts[:-1],starts[1:])]
    return {label:bricks(mask) for label,mask in masks.items()}

def instantiate(bricks,groups=None):
    frames=np.array([b[0] for b in bricks]);result=[]
    for group in (layout()[7] if groups is None else groups):
        for idx in group[1]:
            t,q,s=map(np.array,TRANSFORMS[idx])
            def rotate(a):
                cross=2*np.cross(q[:3],a);return a+q[3]*cross+np.cross(q[:3],cross)
            transformed=rotate(frames*s);transformed[:,0]+=t
            for frame,(_,mask,n) in zip(transformed,bricks):
                norm=rotate(n/s);norm/=np.linalg.norm(norm,axis=1,keepdims=True)
                result.append((frame,mask,norm))
    return result

def partial():
    groups=layout()[7]
    # Nominate the deepest batch along the benchmark view before looking at any image scores.
    # Use one full-size batch so all cost counters have the same placement denominator.
    back=view(132)[2]
    group=min((g for g in groups if len(g[1])==12),key=lambda g:np.mean([TRANSFORMS[i][0] for i in g[1]],0)@back)
    print("partial batch",group[0],group[1].tolist(),flush=True)
    for mm,lb,wb in [(29,8,6),(38,5,3),(58,3,2)]:
        a=instantiate(prepare("part",mm,0,lb)["transfer"],[group])
        b=instantiate(prepare("part",mm,1,wb)["transfer"],[group])
        for joint in (False,True):
            name=f"partial-{mm}-"+("joint" if joint else "leaf")
            write_bake(ROOT/(name+".oakvx"),[a,b if joint else []],3 if joint else 1,[group])

def bake(mm,group):
    leaf=prepare(group,mm,0,3305 if group=="merged" else 9)
    wood=prepare(group,mm,1,2865 if group=="merged" else 8)
    for label,bricks in leaf.items():
        if group=="part" and label=="free":continue
        a,b=(bricks,wood[label]) if group=="merged" else (instantiate(bricks),instantiate(wood[label]))
        for joint in (False,True):
            name=f"{group}-{mm}-{label}-"+("joint" if joint else "leaf")
            write_bake(ROOT/(name+".oakvx"),[a,b if joint else []],3 if joint else 1)
            print(name,len(a),len(b) if joint else 0,flush=True)

if __name__=="__main__":
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument("--mm",type=int,nargs="+",default=[29,38,19,58])
    parser.add_argument("--group",choices=["part","merged","partial"],default="merged")
    args=parser.parse_args()
    if args.group=="partial":partial()
    else:
        for mm in args.mm:bake(mm,args.group)
