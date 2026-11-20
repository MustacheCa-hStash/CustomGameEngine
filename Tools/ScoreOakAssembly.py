"""Screen group reductions against the same 950-triangle/spray reference."""
import argparse
from pathlib import Path
import numpy as np
from CompareTwigParents import raster
from ReplayOakAssembly import OUT, UE, mesh, measure, write
import json

FOCUS=np.array(json.loads((UE.parent/"spray-and-camera.json").read_text())["center_m"])

def rasterize(sample,d,yaw):
    return raster(sample,FOCUS,yaw,-5.46 if d==41 else -2.235,d,False)[0]

def score(files, name, yaws=(132,)):
    rows=[]
    ref=mesh(OUT/"wood.mesh")
    for d in (41,100):
        for yaw in yaws:
            rz=rasterize(ref,d,yaw)
            occupied=np.isfinite(rz)
            for f in files:
                v,i=mesh(f)
                z=rasterize((v,i),d,yaw)
                mask=np.isfinite(z)
                m=next(measure(occupied,[(0,len(i),mask)]))
                both=occupied&mask
                error=np.abs(z[both]-rz[both])*1000
                rows.append(dict(d=d,yaw=yaw,cut=f.relative_to(OUT).as_posix(),triangles=len(i),
                                 iou=m[2],edge=m[-1],p50_mm=np.median(error),p95_mm=np.percentile(error,95)))
    write(name,rows)

if __name__=="__main__":
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument("output")
    parser.add_argument("patterns",nargs="+")
    parser.add_argument("--yaws",nargs="+",type=int,default=[132])
    a=parser.parse_args()
    paths=sorted({p for pat in a.patterns for p in OUT.glob(pat)})
    assert paths
    score(paths,a.output,a.yaws)
