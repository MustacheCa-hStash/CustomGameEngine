"""Screen reusable local-space templates and collect same-camera native controls."""
import argparse
from collections import Counter,defaultdict
import json
import numpy as np
from ReusableOakSprays import ROOT,OUT,UE,EXPOSED,expand_parts,mesh,save,table
from CompareOakJoint import front
from ScoreOakAssembly import FOCUS
from ReplayOakAssembly import measure,read_mask,quality
from DecodeNaniteOakCut import decode_cluster,extract,records

def controls():
    dest=ROOT/"native-part";dest.mkdir(exist_ok=True)
    for b in json.loads((ROOT/"regions.json").read_text()):
        for d in (41,100):
            dump=ROOT/f"joint{b}-cut-{d}"
            if not (dump/"Base/ResourceDescs.json").exists():continue
            out=ROOT/f"live{b}-{d}.mesh"
            print(b,d,extract(dump,out),flush=True)
    # Recover part-local alternatives from the exposed bough only. Each selection
    # is one distinct set of cluster IDs, independent of instance ID or camera.
    for d in (41,100):
        dump=OUT/f"joint-dump{d}"
        desc=next(r for r in records(dump/"Base/ResourceDescs.json") if r["Name"]=="Nanite.StreamingManager.ClusterPageData")
        data=np.memmap(next((dump/"Resources").glob(desc["UniqueResourceName"]+".*.bin")),mode="r",dtype="u1")
        trace=json.loads((OUT/f"joint-nanite{d}.clusters.json").read_text())
        by=defaultdict(list)
        for r in trace:
            if r["instance"]!=0xffffff:by[r["instance"]].append((r["page"],r["cluster"]))
        for ids in sorted(set(tuple(sorted(v)) for v in by.values())):
            vv=[];ii=[];slots=[];offset=0
            for p,c in ids:
                addr=(p<<17) if p<4096 else (512<<20)+((p-4096)<<15)
                pos,n,uv,uv1,tri,s=decode_cluster(data,addr,c)
                vv.append(np.column_stack([pos[:,[0,2,1]]*.01,n[:,[0,2,1]],uv,uv1]))
                ii.append(tri+offset);offset+=len(pos);slots.extend(s)
            name=dest/f"part-{len(slots)}.mesh";save(name,np.vstack(vv),np.vstack(ii))
            name.with_suffix(".mat").write_bytes(np.array(slots,"u1").tobytes())

def slots(path):
    v,tri=mesh(path); labels=np.fromfile(path.with_suffix(".mat"),np.uint8)
    assert len(labels)==len(tri) and np.all(labels<=1)
    return v,tri,labels

def depth(v,tri,labels,d,yaw,crop):
    y,p=np.radians([yaw,-5.46 if d==41 else -2.235])
    back=np.array([np.sin(y)*np.cos(p),np.sin(p),np.cos(y)*np.cos(p)])
    right=np.array([np.cos(y),0,-np.sin(y)])
    axes=np.stack([right,np.cross(back,right),back])
    view=(v[:,:3]-FOCUS)@axes.T
    z=d-view[:,2]; xy=view[:,:2]*(2607.350647/z)[:,None]*[1,-1]+[1920-crop[0],1080-crop[1]]
    width,height=crop[2]-crop[0],crop[3]-crop[1]
    screen=np.full(width*height,np.inf,np.float32)
    faces=tri[(labels==0)|front(v,tri,d,yaw)]
    for ix in np.array_split(faces,max(1,len(faces)//8192)):
        if not len(ix):continue
        uv=xy[ix]; lo=np.maximum(np.ceil(uv.min(1)-.5).astype(int),0);hi=np.minimum(np.floor(uv.max(1)-.5).astype(int),[width-1,height-1])
        sz=np.maximum(hi-lo+1,0);n=sz.prod(1)
        f=np.repeat(np.arange(len(ix)),n)
        off=np.arange(len(f))-np.repeat(np.cumsum(n)-n,n)
        x=lo[f,0]+off%sz[f,0];yy=lo[f,1]+off//sz[f,0]
        a,b,c=uv.transpose(1,0,2);u=b-a;w=c-a
        dx=x+.5-a[f,0];dy=yy+.5-a[f,1]
        det=(u[:,0]*w[:,1]-u[:,1]*w[:,0])[f]
        with np.errstate(divide="ignore",invalid="ignore"):
            s=(dx*w[f,1]-dy*w[f,0])/det;t=(u[f,0]*dy-u[f,1]*dx)/det
            hit=(s>=0)&(t>=0)&(s+t<=1)
        s,t,j=s[hit],t[hit],ix[f[hit]]
        vdepth=1/((1-s-t)/z[j[:,0]]+s/z[j[:,1]]+t/z[j[:,2]])
        np.minimum.at(screen,yy[hit]*width+x[hit],vdepth.astype(np.float32))
    return screen.reshape(height,width)

def ue_images(b,d,yaw):
    tag="joint-cut" if b==7 and yaw in (132,90,180) else f"joint{b}-cut"
    suffix="" if yaw==132 else f"-yaw{yaw}"
    return UE/f"{tag}-{d}{suffix}.png",UE/f"{tag}-{d}{suffix}-fine.png"

def setup(b,d,yaw):
    p,f=ue_images(b,d,yaw)
    ref=read_mask(f,True);cut=read_mask(p,True)
    yy,xx=np.where(ref|cut)
    crop=[max(xx.min()-32,0),max(yy.min()-32,0),min(xx.max()+33,3840),min(yy.max()+33,2160)]
    return crop,quality(p,ref,True)

def projected(path,ids,d,yaw,crop):
    v,i,s=slots(path)
    v,i=expand_parts((v,i),ids)
    return depth(v,i,np.tile(s,len(ids)),d,yaw,crop)

def score():
    regions=json.loads((ROOT/"regions.json").read_text())
    files=list(ROOT.glob("epic-*/level*.mesh"))+list(ROOT.glob("custom-*/level*.mesh"))+list(ROOT.glob("native-part/*.mesh"))
    candidates=[(p,len(mesh(p)[1])) for p in files]
    rows=[]
    for d in (41,100):
        valid=[(p,n) for p,n in candidates if (210<=n<=850 if d==41 else 65<=n<=400)]
        for yaw in (132,90,180):
            for key,ids in regions.items():
                b=int(key);crop,(ti,te)=setup(b,d,yaw)
                ref=projected(ROOT/"spray.mesh",ids,d,yaw,crop);mask=np.isfinite(ref)
                for p,n in valid:
                    z=projected(p,ids,d,yaw,crop);occ=np.isfinite(z)
                    m=next(measure(mask,[(0,0,occ)]))
                    shared=occ&mask
                    rows.append(dict(d=d,b=b,yaw=yaw,cut=p.relative_to(ROOT).as_posix(),per_spray=n,tri=n*len(ids),
                                     iou=m[2],edge=m[-1],ue_iou=ti,ue_edge=te,passed=m[2]>=ti and m[-1]<=te,
                                     p95_mm=np.percentile(abs(z[shared]-ref[shared])*1000,95)))
            table("template-screen.csv",rows)
            if yaw==132:
                valid=[(p,n) for p,n in valid if any(r["cut"]==p.relative_to(ROOT).as_posix() and r["d"]==d and
                    r["iou"]>=r["ue_iou"]-.009 and r["edge"]<=r["ue_edge"]+.008 for r in rows)]

if __name__=="__main__":
    p=argparse.ArgumentParser();p.add_argument("task",choices=["controls","score"]);args=p.parse_args()
    {"controls":controls,"score":score}[args.task]()
