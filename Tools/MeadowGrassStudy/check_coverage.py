"""Measure registered 4K grass cuts and isolate blade-to-bark color transport."""
import argparse
import csv
import json
import os
from pathlib import Path
import shutil
import tempfile

import numpy as np
from PIL import Image

import oak_baseline as base
import coverage as study


def bounce_test():
    out=study.OUT/"bounce-test"
    out.mkdir(exist_ok=True)
    source=study.OUT/"layered/scene"
    focus=json.loads((source/"metadata.json").read_text())["focus"]
    with tempfile.TemporaryDirectory(dir=out) as tmp:
        stage=Path(tmp)/"scene"
        shutil.copytree(source,stage)
        for recolor in (False,True):
            if recolor:
                model=base.oak.OakGlb(stage/"assets"/base.MODEL.name)
                for mat in model.doc["materials"]:
                    if mat["name"].startswith("Lamina |"):
                        mat["pbrMetallicRoughness"]["baseColorFactor"]=[1,0,1,1]
                        mat["extensions"]["KHR_materials_diffuse_transmission"]["diffuseTransmissionColorFactor"]=[1,0,1]
                model.write(stage/"assets"/base.MODEL.name)
            for local in (False,True):
                arm=("local" if local else "uniform")+("-recolored" if recolor else "-normal")
                env=os.environ.copy()
                env.pop("MEADOW_UNIFORM_BOUNCE",None)
                if not local:
                    env["MEADOW_UNIFORM_BOUNCE"]="1"
                dds=Path(tmp)/(arm+".dds")
                base.run("edge",focus,"capture","combined",False,viewer=stage/"Meadow.exe",output_dir=out/arm,
                         environment=env,extra_args=["--linear-hdr",str(dds)])
                hdr=np.memmap(dds,dtype="<f2",mode="r",offset=148,shape=(2160,3840,4))
                # Same exposed bark window as the earlier negative control.
                for label,(x,y,w,h) in {"bark":(3280,755,140,160),"grass":(1400,1380,350,350)}.items():
                    np.save(out/arm/(label+".npy"),np.array(hdr[y:y+h,x:x+w,:3]))
                del hdr
    metrics={}
    for arm in ("uniform","local"):
        for label in ("bark","grass"):
            normal=np.load(out/(arm+"-normal")/(label+".npy")).astype(float)
            red=np.load(out/(arm+"-recolored")/(label+".npy")).astype(float)
            delta=(red-normal).mean(axis=(0,1))
            metrics[arm+"-"+label]=dict(linearMean=normal.mean(axis=(0,1)).tolist(),
                    deltaLinear=delta.tolist(),changedPixels=int(np.any(red!=normal,axis=2).sum()),
                    greenChangePct=float(100*delta[1]/normal[:,:,1].mean()))
    (out/"measurements.json").write_text(json.dumps(metrics,indent=2))
    print(json.dumps(metrics,indent=2))


def compare_geometry():
    report={}
    for p in ("edge","roots","blade"):
        def read(a):
            return np.asarray(Image.open(study.OUT/a/(p+".png")).convert("RGB"),dtype=np.int16)
        diff=np.abs(read("optimized")-read("layered"))
        report[p]=dict(meanAbsolute8bit=float(diff.mean()),p99LargestChannel=float(
            np.percentile(diff.max(axis=2),99)))
    times={}
    for repeat,arm in enumerate(("layered","optimized","optimized","layered")):
        scene=study.OUT/arm/"scene"
        dest=study.OUT/"timings"/str(repeat)
        focus=json.loads((scene/"metadata.json").read_text())["focus"]
        base.run("edge",focus,"capture","combined",False,viewer=scene/"Meadow.exe",output_dir=dest,
                 extra_args=["--timing-samples","20"])
        with (dest/"edge.timings.csv").open() as f:
            times.setdefault(arm,[]).extend(float(r["gpu_frame_ms"]) for r in csv.DictReader(f))
    report["gpuFrameMedianMs"]={k:float(np.median(v)) for k,v in times.items()}
    (study.OUT/"geometry-results.json").write_text(json.dumps(report,indent=2))
    print(report)


if __name__=="__main__":
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument("test",choices=("bounce","geometry"))
    args=ap.parse_args()
    (bounce_test if args.test=="bounce" else compare_geometry)()
