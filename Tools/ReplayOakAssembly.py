"""Stage and evaluate one wood-only assembly cut using the existing matched oak fixtures."""
import csv
from functools import lru_cache
import hashlib
import json
import os
from pathlib import Path
import shutil
import struct
import subprocess
import uuid
from statistics import median

import numpy as np
from CompareOakMaskEdges import read_mask, measure


REPO = Path(__file__).resolve().parents[1]
BUILD = REPO/"build"
PREVIOUS = BUILD/"oak-branch-step-final"
OUT = BUILD/"oak-assembly-step"
UE = BUILD/"unreal-nanite-oak/unreal"
TREE = "WhiteOak-ForkedCanopy.treelod"


class Hierarchy:
    def __init__(self, data):
        self.header = data[:32]
        at = 32
        def read(size):
            nonlocal at
            n, = struct.unpack_from("<I",data,at)
            at += 4
            begin = at
            at += size*n
            return data[begin:at]
        self.order = read(4)
        count, = struct.unpack_from("<I",data,at)
        at += 4
        self.parts = []
        for _ in range(count):
            mesh = data[at:at+4]
            at += 4
            self.parts.append([mesh, bytearray(read(56)), read(40), read(4)])
        self.tail = data[at:]

    def write(self, path):
        with path.open("wb") as f:
            def array(raw, size):
                f.write(struct.pack("<I",len(raw)//size))
                f.write(raw)
            f.write(self.header)
            array(self.order,4)
            f.write(struct.pack("<I",len(self.parts)))
            for m,n,v,i in self.parts:
                f.write(m)
                for raw,size in ((n,56),(v,40),(i,4)):array(raw,size)
            f.write(self.tail)


def mesh(path):
    data = path.read_bytes()
    nv,ni=struct.unpack_from("<2I",data)
    assert len(data)==8+40*nv+4*ni and ni%3==0
    return (np.frombuffer(data,"<f4",nv*10,8).reshape(-1,10),
            np.frombuffer(data,"<u4",ni,8+40*nv).reshape(-1,3))


def viewer_assets(name, source):
    dest = OUT/name
    assets = dest/"assets"
    assets.mkdir(parents=True,exist_ok=True)
    for f in (source/"assets").iterdir():
        if f.name==TREE or (assets/f.name).exists():continue
        try:os.link(f,assets/f.name)
        except OSError:shutil.copy2(f,assets/f.name)
    if not (dest/"CustomGameEngine.exe").exists() or (dest/"CustomGameEngine.exe").stat().st_mtime_ns!=(BUILD/"Release/CustomGameEngine.exe").stat().st_mtime_ns:
        shutil.copy2(BUILD/"Release/CustomGameEngine.exe",dest)
    return dest


def stage(name, geometry, whole=False, source=None):
    source = source or PREVIOUS/("grid" if whole else "iso-grid")
    dest = viewer_assets(name,source)
    h=Hierarchy((source/"assets"/TREE).read_bytes())
    v,i=mesh(geometry)
    if whole:
        part=h.parts[1]
        first=len(part[3])//4
        part[3]+= (i+len(part[2])//40).astype("<u4").tobytes()
        part[2]+=v.tobytes()
        struct.pack_into("<IIff",part[1],2*56+24,first,i.size,0.,0.)
    else:
        for j,part in enumerate(h.parts):
            n=part[1][:56]
            struct.pack_into("<IIffII",n,24,0,i.size if j==1 else 3,0.,0.,0,0)
            part[1:]=[n,v.tobytes() if j==1 else bytes(40),i.tobytes() if j==1 else bytes(12)]
    h.write(dest/"assets"/TREE)
    return dest


@lru_cache(maxsize=128)
def file_digest(path, size, modified):
    with path.open("rb") as stream:
        digest = hashlib.sha256()
        for chunk in iter(lambda: stream.read(1024*1024), b""):
            digest.update(chunk)
        return digest.hexdigest()


def capture(viewer,tag,d,yaw=132,budget=None,shaded=False,one_sided=False,mixed=False):
    folder=OUT/"captures"
    folder.mkdir(exist_ok=True)
    b=budget if budget is not None else (1.02211 if d==41 else .49018)
    suffix="-mixed" if mixed else "-onesided" if one_sided else ""
    path=folder/f"{tag}-{d}-{yaw}-{b}{suffix}.png"
    samples=61 if shaded else 3
    args=[viewer/"CustomGameEngine.exe","--capture",path,"--scene","forked-oak","--camera","overview",
          "--width","3840","--height","2160","--msaa","1","--distance",str(d),"--yaw-degrees",str(yaw),
          "--pitch-degrees","-5.46" if d==41 else "-2.235","--wind-strength","0","--wind-time","0",
          "--lod","auto","--spray-mode","hierarchy","--tree-submit","direct","--spray-culling","on",
          "--spray-local-lod","on","--spray-pixel-error",str(b),"--lighting","combined","--probe-gi","off",
          "--timing-samples",str(samples)]
    if not shaded:args.append("--geometry-mask")
    if one_sided:args.append("--cull-mesh-backfaces")
    if mixed:args.append("--cull-spray-wood-backfaces")
    def identity(file):
        stat = file.stat()
        return file_digest(file,stat.st_size,stat.st_mtime_ns)
    signature = dict(command=[str(a) for a in args[3:]], executable=identity(args[0]),
                     hierarchy=identity(viewer/"assets"/TREE))
    record = path.with_suffix(".inputs.json")
    if (path.exists() or path.with_suffix(".timings.csv").exists()) and (
            not record.exists() or json.loads(record.read_text()) != signature):
        stamp=hashlib.sha256(json.dumps(signature,sort_keys=True).encode()).hexdigest()[:12]
        path=path.with_name(f"{path.stem}-{stamp}.png")
        record=path.with_suffix(".inputs.json")
    if not (path.exists() and path.with_suffix(".timings.csv").exists() and record.exists()
            and json.loads(record.read_text()) == signature):
        pending=path.with_name(f"{path.stem}-{uuid.uuid4().hex}.png")
        args[2]=pending
        with path.with_suffix(".log").open("w") as log:
            subprocess.run(args,check=True,stdout=log,stderr=log,timeout=90)
        pending.replace(path)
        pending.with_suffix(".timings.csv").replace(path.with_suffix(".timings.csv"))
        temporary=pending.with_suffix(".json")
        temporary.write_text(json.dumps(signature,indent=2))
        temporary.replace(record)
    rows=list(csv.DictReader(path.with_suffix(".timings.csv").open()))
    assert len(rows)==samples
    return path,dict(triangles=sum(int(rows[0][k]) for k in ("camera_leaf_triangles","camera_spray_wood_triangles","camera_tree_trunk_triangles")),
                     wood=int(rows[0]["camera_spray_wood_triangles"]),ms=median(float(r["gpu_camera_ms"]) for r in rows))


def quality(path, ref, unreal=False):
    result=next(measure(ref,[(0,0,read_mask(path,unreal))]))
    return float(result[2]),float(result[-1])


def reference(d,yaw=132,whole=False):
    if whole:return read_mask(BUILD/f"oak-triangles-matched/fixed_q{d}.png")
    return read_mask(next((PREVIOUS/"captures").glob(f"iso-ref-{d}-*-mask-{yaw}-direct.png")))


def write(name, rows):
    with (OUT/name).open("w",newline="") as f:
        w=csv.DictWriter(f,fieldnames=list(rows[0]))
        w.writeheader();w.writerows(rows)


def live():
    rows=[]
    for d in (41,100):
        geometry=OUT/f"nanite{d}.mesh"
        iso=stage(f"live{d}-wood",geometry)
        full=stage(f"live{d}-tree",geometry,True)
        for whole,viewer in [(False,iso),(True,full)]:
            tag=viewer.name
            p,r=capture(viewer,tag,d)
            iou,edge=quality(p,reference(d,whole=whole))
            ue_cut=read_mask(UE/f"wood-cut-{d}.png",True)
            cross_iou=quality(p,ue_cut)[0] if not whole else None
            times=[capture(viewer,tag+f"-shaded{j}",d,shaded=True)[1]["ms"] for j in range(4)]
            rows.append(dict(d=d,scope="whole" if whole else "wood",triangles=r["triangles"] if whole else r["wood"],
                             iou=iou,edge=edge,cross_cut_iou=cross_iou,ms=median(times)))
    write("live-replay.csv",rows)


if __name__=="__main__":live()
