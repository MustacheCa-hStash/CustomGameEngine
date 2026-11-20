"""Prepare matched local-space spray cuts and recover the two boughs from the oak skeleton."""
import csv
import json
import struct
from pathlib import Path
import numpy as np
from PrepareOakAssembly import load_part, save
from ReplayOakAssembly import BUILD, OUT, REPO, UE, mesh
from CompareOakJoint import front
from ScoreOakAssembly import rasterize

ROOT = OUT/"reusable"
TRANSFORMS = json.loads((UE.parent/"placements.json").read_text())
EXPOSED = 7
YAWS = (132,90,180)

def table(name, rows):
    with (ROOT/name).open("w",newline="") as f:
        w=csv.DictWriter(f,fieldnames=list(rows[0])); w.writeheader(); w.writerows(rows)

def expand_parts(geometry, ids):
    v,ix=geometry
    p,q,s=[np.array([TRANSFORMS[k][col] for k in ids])[:,None,:] for col in range(3)]
    def rotate(x):
        t=2*np.cross(q[:,:,:3],x)
        return x+q[:,:,3:]*t+np.cross(q[:,:,:3],t)
    vout=np.empty((len(ids),len(v),10),np.float32)
    vout[:,:,:3]=rotate(v[:,:3]*s)+p
    n=rotate(v[:,3:6]/s)
    vout[:,:,3:6]=n/np.linalg.norm(n,axis=2,keepdims=True)
    vout[:,:,6:]=v[:,6:]
    ix=np.where((s.prod(2)<0)[:,:,None],ix[:,[0,2,1]],ix)
    return vout.reshape(-1,10),(ix+np.arange(len(ids))[:,None,None]*len(v)).astype(np.uint32).reshape(-1,3)

def source_trunk():
    glb=(REPO/"assets/WhiteOak-ForkedCanopy.glb").read_bytes()
    size,=struct.unpack_from("<I",glb,12)
    doc=json.loads(glb[20:20+size]); payload=memoryview(glb)[28+size:]
    p=doc["meshes"][0]["primitives"][0]
    def read(key,columns):
        a=doc["accessors"][key];b=doc["bufferViews"][a["bufferView"]]
        dtype=np.dtype({5126:"<f4",5123:"<u2",5125:"<u4"}[a["componentType"]])
        return np.ndarray((a["count"],columns),dtype,payload,b.get("byteOffset",0)+a.get("byteOffset",0),
                          (b.get("byteStride",columns*dtype.itemsize),dtype.itemsize))
    xyz=read(p["attributes"]["POSITION"],3)
    v=np.zeros((len(xyz),10),np.float32);v[:,:3]=xyz
    return v,read(p["indices"],1).reshape(-1,3)

def membership():
    sk=json.loads((BUILD/"oak-pve-forked-test/skeleton/WhiteOak.json").read_text())
    pts=np.array(sk["points"]["positions"])
    ancestors=sk["primitives"]["attributes"]["parents"]["values"]
    ids=np.zeros(len(pts),int)
    for b,seq in enumerate(sk["primitives"]["points"]):
        path=ancestors[b]+[b+1]
        ids[seq[1:]]=path[min(1,len(path)-1)]
    assigned=[]
    for x in np.array_split(np.array([t[0] for t in TRANSFORMS]),100):
        assigned.extend(ids[((x[:,None]-pts)**2).sum(2).argmin(1)])
    assigned=np.array(assigned)
    known=list(map(int,(REPO/"assets/source/white-oak-forked-spray/WhiteOak-Bough07.txt").read_text().split()[1:]))
    assert np.array_equal(np.flatnonzero(assigned==EXPOSED),known)
    return assigned

def prepare():
    ROOT.mkdir(exist_ok=True)
    source=[load_part(i) for i in (0,1)]
    (v,i),(w,t)=source
    save(ROOT/"spray.mesh",np.vstack([v,w]),np.vstack([i,t+len(v)]))
    (ROOT/"spray.mat").write_bytes(bytes(len(i))+bytes([1])*len(t))
    jobs=[dict(input="reusable/spray.mesh",arm=a,ratio=r,output=f"reusable/{a}-{r}")
          for a in ("epic","custom") for r in (.38,.43,.48,.5,.55,.6,.65,.7,.75,.8,.85)]
    (ROOT/"jobs.json").write_text(json.dumps(jobs))
    assigned=membership()
    # Stream source bough depths, including the trunk and the smaller shoots.
    # Compare only frontmost sample ownership with the rest of the complete tree.
    ranks=[]
    for d in (41,100):
        trunk=rasterize(source_trunk(),d,132)
        sy,sx=np.where(np.isfinite(trunk)); margin=round(750*41/d)
        crop=np.s_[max(0,sy.min()-margin):min(2160,sy.max()+margin+1),
                   max(0,sx.min()-margin):min(3840,sx.max()+margin+1)]
        full=trunk[crop].copy()
        buffers={}
        for b in np.unique(assigned):
            ids=np.flatnonzero(assigned==b)
            z=full.copy(); z[:]=np.inf
            for slot,part in enumerate(source):
                vv,ii=expand_parts(part,ids)
                if slot==1:ii=ii[front(vv,ii,d,132)]
                raw=rasterize((vv,ii),d,132)
                assert np.isfinite(raw).sum()==np.isfinite(raw[crop]).sum()
                np.minimum(z,raw[crop],out=z)
            buffers[b]=z
            np.minimum(full,z,out=full)
        for b,z in buffers.items():
            covered=np.isfinite(z);hits=(covered & (z==full)).sum()
            ranks.append(dict(d=d,bough=int(b),placements=int((assigned==b).sum()),isolated_pixels=int(covered.sum()),
                              visible_pixels=int(hits),visible_fraction=hits/covered.sum()))
    table("exposure.csv",ranks)
    comparable=[r["bough"] for r in ranks if r["d"]==41 and 200<=r["placements"]<=450 and r["bough"]!=EXPOSED]
    interior=min(comparable,key=lambda b:np.mean([r["visible_fraction"] for r in ranks if r["bough"]==b]))
    specs={}
    for b in (EXPOSED,interior):
        ids=np.flatnonzero(assigned==b).tolist()
        specs[str(b)]=ids
        root=[]
        for part in source:
            v,ii=expand_parts(part,ids[:1])
            root.append(dict(vertices=(v[:,[0,2,1]]*100).tolist(),normals=v[:,[3,5,4]].tolist(),
                             uv0=v[:,6:8].tolist(),uv1=v[:,8:10].tolist(),triangles=ii.tolist()))
        (ROOT/f"bough{b}-ue.json").write_text(json.dumps(dict(root=root,others=[TRANSFORMS[k] for k in ids[1:]])))
    (ROOT/"regions.json").write_text(json.dumps(specs))
    print("Selected boughs",[(b,len(v)) for b,v in specs.items()])

if __name__=="__main__":prepare()
