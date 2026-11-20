"""Export matched part meshes, placement transforms and instanced assembly inputs."""
import argparse
import json
import struct
import numpy as np
from ReplayOakAssembly import OUT,UE,REPO,mesh

def save(path,v,i):
    path.write_bytes(struct.pack("<2I",len(v),i.size)+np.asarray(v,"<f4").tobytes()+np.asarray(i,"<u4").tobytes())

def load_part(part):
    s=json.loads((UE.parent/"spray-and-camera.json").read_text())["parts"][part]
    return (np.column_stack([np.array(s["vertices"])[:,[0,2,1]]*.01,np.array(s["normals"])[:,[0,2,1]],
                             s["uv0"],s.get("uv1",s["uv0"])]),np.array(s["triangles"]))

def expand(sample):
    v,ix=sample
    ids=list(map(int,(REPO/"assets/source/white-oak-forked-spray/WhiteOak-Bough07.txt").read_text().split()[1:]))
    transforms=json.loads((UE.parent/"placements.json").read_text())
    vs=[];indices=[]
    for j,id in enumerate(ids):
        p,q,s=transforms[id]
        def rot(a):
            t=2*np.cross(q[:3],a)
            return a+q[3]*t+np.cross(q[:3],t)
        n=rot(v[:,3:6]/s);n/=np.linalg.norm(n,axis=1)[:,None]
        vs.append(np.column_stack([rot(v[:,:3]*s)+p,n,v[:,6:]]))
        indices.append((ix[:,[0,2,1]] if np.prod(s)<0 else ix)+j*len(v))
    return np.concatenate(vs),np.concatenate(indices)

def prepare():
    OUT.mkdir(exist_ok=True)
    for part,name in enumerate(("leaf","wood")):
        data=load_part(part)
        save(OUT/(name+"-part.mesh"),*data)
        save(OUT/(name+".mesh"),*expand(data))
    jobs=[dict(input="wood-part.mesh",arm=a,ratio=r,output=f"part-{a}-{r}") for a in ("epic","custom") for r in (.5,.68)]
    (OUT/"part-jobs.json").write_text(json.dumps(jobs))

def joint():
    leaf,wood=mesh(OUT/"leaf.mesh"),mesh(OUT/"wood.mesh")
    save(OUT/"joint.mesh",np.vstack([leaf[0],wood[0]]),np.vstack([leaf[1],wood[1]+len(leaf[0])]))
    (OUT/"joint.mat").write_bytes(bytes(len(leaf[1]))+bytes([1])*len(wood[1]))
    jobs=[dict(input=name+".mesh",arm=a,ratio=.5,output=f"{name}-{a}-mixedtest")
          for name in ("joint","leaf") for a in ("epic","custom")]
    (OUT/"joint-jobs.json").write_text(json.dumps(jobs))
    root=[]
    for slot,(v,tri) in enumerate((leaf,wood)):
        vs,ix=load_part(slot)
        vv=v[:len(vs)]
        root.append(dict(vertices=(vv[:,[0,2,1]]*100).tolist(),normals=vv[:,[3,5,4]].tolist(),
                         uv0=vv[:,6:8].tolist(),uv1=vv[:,8:10].tolist(),triangles=tri[:len(ix)].tolist()))
    (OUT/"joint-root.json").write_text(json.dumps(root,separators=(",",":")))

def seeds():
    jobs=[]
    for arm in ("epic","custom"):
        path=OUT/f"part-{arm}-0.5"
        for lv in ("01","02","03"):
            f=path/f"level{lv}.mesh"
            if not f.exists():continue
            file=f"instanced-{arm}-{lv}.mesh"
            save(OUT/file,*expand(mesh(f)))
            for method in ("epic","custom"):
                jobs.append(dict(input=file,arm=method,ratio=.5,output=f"seed-{arm}{lv}-{method}"))
    (OUT/"seed-jobs.json").write_text(json.dumps(jobs))

if __name__=="__main__":
    p=argparse.ArgumentParser();p.add_argument("action",choices=["prepare","seeds","joint"])
    {"prepare":prepare,"seeds":seeds,"joint":joint}[p.parse_args().action]()
