"""Prepare and inspect the registered coverage and tall-blade comparisons."""
import argparse
import copy
import hashlib
import json
import math
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

import numpy as np

import oak_baseline as base
import refine_lighting as lighting

HERE = Path(__file__).resolve().parent
OUT = base.ROOT/"build/meadow-coverage"
SOURCE = base.TUFT.parent
LOW = SOURCE/"meadow-tuft-folded.glb"
SHORT = OUT/"understory.glb"
ARMS = ("current","soil","layered","optimized")


def write(model,path):
    model.payload.extend(bytes(-len(model.payload)%4))
    model.doc["buffers"] = [{"byteLength":len(model.payload)}]
    model.write(path)


def folded_indices(xyz,uv,rows=8):
    """Keep ridge, two margins and tip. Split the interval with the largest shape error."""
    if len(xyz)!=151 or not np.allclose(np.ptp(uv[:-1,1].reshape(30,5),axis=1),0):
        raise ValueError("Expected a 30-row source lamina and a single tip")
    ribbon = np.vstack([xyz[:-1].reshape(30,5,3)[:,[0,2,4]],np.repeat(xyz[-1:],3,axis=0)[None,:,:]])
    cuts = [0,30]
    while len(cuts)<rows+1:
        errors = np.zeros(31)
        for a,b in zip(cuts,cuts[1:]):
            t = np.arange(1,b-a)[:,None,None]/(b-a)
            predicted = ribbon[a]+(ribbon[b]-ribbon[a])*t
            errors[a+1:b] = np.linalg.norm(ribbon[a+1:b]-predicted,axis=2).max(axis=1)
        cuts = sorted([*cuts,int(np.argmax(errors))])
    ids = [5*i+k for i in cuts[:-1] for k in (0,2,4)]+[150]
    faces = []
    for i in range(rows-1):
        j = i*3
        faces.extend([(j,j+3,j+4),(j,j+4,j+1),(j+1,j+4,j+5),(j+1,j+5,j+2)])
    j = (rows-1)*3
    faces.extend([(j,j+3,j+1),(j+1,j+3,j+2)])
    return ids,np.array(faces,dtype="<u4")


def encode_part(model,streams,faces,mat):
    return dict(attributes={key:base.accessor(model,np.array(v,dtype="<f4")) for key,v in streams.items()},
                indices=base.accessor(model,np.asarray(faces,dtype="<u4").reshape(-1,1)),material=mat)


def build_grass(short):
    original = base.oak.OakGlb(base.TUFT)
    model = copy.deepcopy(original)
    model.payload.clear()
    for k in ("bufferViews","accessors","images","materials","textures","samplers","meshes","nodes"):
        model.doc[k]=[]
    base.add_materials(model,original)
    model.doc["scenes"]=[dict(nodes=[],name="Short grass" if short else "Folded tall grass")]
    model.doc["scene"]=0
    rng = np.random.default_rng(942)
    all_blades = []
    for mesh in original.doc["meshes"]:
        parts = []
        for p in mesh["primitives"]:
            mat = p["material"]
            streams = {k:original.array(a) for k,a in p["attributes"].items()}
            idx = original.array(p["indices"]).reshape(-1,3)
            if not original.doc["materials"][mat]["name"].startswith("Lamina |"):
                if not short:
                    parts.append(encode_part(model,streams,idx,mat))
                continue
            count = len(streams["POSITION"])//151
            if np.any(np.ptp(idx//151,axis=1)) or len(idx)!=count*236:
                raise ValueError("Unexpected connectivity in a source blade")
            batches,faces = [],[]
            for i in range(count):
                batch = {k:v[i*151:(i+1)*151] for k,v in streams.items()}
                ids,tri = folded_indices(batch["POSITION"],batch["TEXCOORD_0"],6 if short else 8)
                blade = {k:v[ids] for k,v in batch.items()}
                if short:
                    all_blades.append((blade,tri,mat))
                else:
                    faces.append(tri+i*len(ids))
                    batches.append(blade)
            if not short:
                parts.append(encode_part(model,{k:np.concatenate([v[k] for v in batches]) for k in streams},
                                         np.concatenate(faces),mat))
        if parts:
            model.doc["meshes"].append(dict(name=mesh["name"],primitives=parts))
    if short:
        groups = {}
        for index in rng.choice(len(all_blades),28,replace=False):
            blade,tri,mat = all_blades[index]
            pos = blade["POSITION"]
            origin = pos[:3].mean(axis=0)
            pos = (pos-origin)*rng.uniform(0.34,0.57)
            pos += np.array([rng.normal(0,0.024),0,rng.normal(0,0.024)])
            blade["POSITION"] = pos
            groups.setdefault(mat,[]).append((blade,tri))
        parts = []
        for mat,items in groups.items():
            data = {k:np.concatenate([v[k] for v,_ in items]) for k in items[0][0]}
            faces = np.concatenate([tri+i*len(v["POSITION"]) for i,(v,tri) in enumerate(items)])
            parts.append(encode_part(model,data,faces,mat))
        model.doc["meshes"].append(dict(name="Short grass",primitives=parts))
    model.doc["nodes"]=[dict(mesh=i,name=m["name"]) for i,m in enumerate(model.doc["meshes"])]
    model.doc["scenes"][0]["nodes"]=list(range(len(model.doc["nodes"])))
    write(model,SHORT if short else LOW)


def short_scatter():
    rng = np.random.default_rng(6859)
    tall = np.array(base.scatter()["TRANSLATION"])[:,[0,2]]
    trs = dict(TRANSLATION=[],ROTATION=[],SCALE=[])
    for _ in range(4300):
        x,z = rng.uniform([-9.9,-0.7],[5.8,5.15])
        if ((x+2.05)/7.65)**2+((z-2.18)/2.73)**2>1 or np.hypot(x,z)<1.05:
            continue
        patch = 0.5+0.23*np.sin(x*1.73+z*0.84)+0.22*np.cos(z*2.5-x*0.62)
        if rng.random()>patch or np.min(np.sum((tall-[x,z])**2,axis=1))<0.085**2:
            continue
        s = float(rng.uniform(0.65,1.18))
        a = float(rng.uniform(0,np.pi))
        trs["TRANSLATION"].append((float(x),-0.006,float(z)))
        trs["ROTATION"].append((0,math.sin(a),0,math.cos(a)))
        trs["SCALE"].append((s,s,s))
    return trs


def copy_file(src,dst):
    if not dst.exists() or src.stat().st_size!=dst.stat().st_size or src.stat().st_mtime_ns!=dst.stat().st_mtime_ns:
        shutil.copy2(src,dst)


def build_variant(arm):
    folder=OUT/arm/"scene/assets"
    folder.mkdir(parents=True,exist_ok=True)
    copy_file(OUT/"compiler/Release/CustomGameEngine.exe",folder.parent/"Meadow.exe")
    for path in base.ASSETS.glob("MeadowDaySky.*"):
        copy_file(path,folder/path.name)
    if arm!="current":
        copy_file(SOURCE/"meadow-soil-thatch.png",folder/"MeadowSoil.png")
    layered=arm in ("layered","optimized")
    visibility=OUT/"LayeredVisibility.png" if layered else base.ASSETS/"MeadowGroundVisibility.png"
    copy_file(visibility,folder/"MeadowGroundVisibility.png")
    model=base.oak.OakGlb(base.CROWN)
    counts={}
    for name,path,trs in [("Full grass strip",LOW if arm=="optimized" else base.TUFT,base.scatter()),
                          ("Short grass",SHORT,short_scatter())]:
        if name=="Short grass" and not layered:
            continue
        asset=base.oak.OakGlb(path)
        offset=len(model.doc["materials"])
        base.add_materials(model,asset)
        mesh,count=base.tuft_mesh(model,asset,offset)
        attr={k:base.accessor(model,np.array(v,dtype="<f4")) for k,v in trs.items()}
        node=dict(name=name,mesh=mesh,extensions={"EXT_mesh_gpu_instancing":{"attributes":attr}})
        model.doc["scenes"][0]["nodes"].append(len(model.doc["nodes"]))
        model.doc["nodes"].append(node)
        counts[name]=[len(trs["SCALE"]),count]
    write(model,folder/base.MODEL.name)
    (folder.parent/"metadata.json").write_text(json.dumps(dict(focus=base.world_center(model).tolist(),counts=counts)))


def bake_visibility():
    doc={"layers":[dict(asset=str(path),placements=trs) for path,trs in
                   [(base.TUFT,base.scatter()),(SHORT,short_scatter())]]}
    script=HERE/"bake_ground_visibility.py"
    data=json.dumps(doc).encode()
    digest=hashlib.sha256(data+script.read_bytes()+base.TUFT.read_bytes()+SHORT.read_bytes()).hexdigest()
    stamp=OUT/"visibility.sha256"
    dest=OUT/"LayeredVisibility.png"
    if dest.exists() and stamp.exists() and stamp.read_text()==digest:
        return
    blender=shutil.which("blender") or Path(os.environ["ProgramFiles"])/"Blender Foundation/Blender 5.0/blender.exe"
    with tempfile.TemporaryDirectory(dir=OUT) as tmp:
        manifest=Path(tmp)/"scatter.json"
        manifest.write_bytes(data)
        output=Path(tmp)/dest.name
        subprocess.run([blender,"-b","--python-exit-code","1","-P",script,"--",manifest,output],check=True)
        output.replace(dest)
    stamp.write_text(digest)


def run(arm,mode,pose,env=None):
    stage=OUT/arm/"scene"
    meta=json.loads((stage/"metadata.json").read_text())
    base.run(pose,meta["focus"],mode,"combined",False,viewer=stage/"Meadow.exe",
             output_dir=OUT/arm,environment=env)


if __name__=="__main__":
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument("action",choices=("prepare","capture","view"))
    ap.add_argument("--arm",choices=ARMS,default="optimized")
    ap.add_argument("--pose",choices=base.VIEWS,default="edge")
    args=ap.parse_args()
    if args.action=="prepare":
        OUT.mkdir(parents=True,exist_ok=True)
        base.stage()
        if not (lighting.OUT/"source").exists():
            lighting.prepare_viewer()
        if not (base.ASSETS/"MeadowGroundVisibility.png").exists():
            lighting.ground_visibility()
        lighting.prepare_viewer(upstream=lighting.OUT/"source",out=OUT,patch=HERE/"coverage.patch",
                                destination=OUT/"Meadow.exe")
        build_grass(True)
        if args.arm in ("layered","optimized"):
            bake_visibility()
        if args.arm=="optimized":
            build_grass(False)
        build_variant(args.arm)
    else:
        run(args.arm,args.action,args.pose)
