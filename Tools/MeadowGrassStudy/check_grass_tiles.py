"""Capture registered 4K culling controls, independent caster checks and the meadow revision."""
import argparse
import csv
import json
from statistics import median

import numpy as np
from PIL import Image, ImageDraw, ImageFont

import grass_tiles as study


OUT = study.OUT/"review"
POSES = ("canopy","clearing")
ARMS = study.VARIANTS


def benchmark():
    study.prepare()
    for run,order in enumerate((ARMS,tuple(reversed(ARMS)))):
        for arm in order:
            for pose in POSES:
                target = OUT/arm if run==0 else OUT/arm/"repeat"
                study.capture(arm,pose,"capture",directory=target)
    controls = [
        ("before","before",None),("culled","culled",None),("meadow","meadow",None),
        ("sorted-unculled","culled","MEADOW_TEST_KEEP_ALL_TILES"),
        ("camera-only-tiles","culled","MEADOW_TEST_CAMERA_SHADOWS"),
        ("camera-only-tufts","culled","MEADOW_TEST_CAMERA_TUFTS"),
        ("no-oak-shadow","culled","MEADOW_TEST_NO_OAK_SHADOW"),
    ]
    for label,arm,flag in controls:
        for pose in POSES:
            study.capture(arm,pose,"capture","shadow-visibility",OUT/label,flag)
    for pose in POSES:
        study.capture("culled",pose,"capture",directory=OUT/"sorted-unculled",diagnostic="MEADOW_TEST_KEEP_ALL_TILES")
    study.capture("culled","clearing","capture",directory=OUT/"no-oak-camera",diagnostic="MEADOW_TEST_NO_OAK_CAMERA")


def image_error(first,second):
    a = np.asarray(Image.open(first),dtype=np.int16)
    b = np.asarray(Image.open(second),dtype=np.int16)
    diff = np.abs(a-b).max(axis=2)
    return dict(changedPixels=int((diff>0).sum()),changedByAtLeast8=int((diff>=8).sum()),maxChannel=int(diff.max()))


def panels(path,items,width,crop=None):
    font=ImageFont.truetype("C:/Windows/Fonts/segoeui.ttf",max(19,width//34))
    height=round(width*((crop[3]-crop[1])/(crop[2]-crop[0]) if crop else 9/16))
    header=max(43,width//19)
    plate=Image.new("RGB",(len(items)*width,height+header),(27,29,26))
    draw=ImageDraw.Draw(plate)
    for i,(label,source) in enumerate(items):
        with Image.open(source) as frame:
            if crop:
                frame=frame.crop(crop)
            plate.paste(frame.resize((width,height),Image.Resampling.LANCZOS),(i*width,header))
        draw.text((i*width+12,header//2),label,font=font,fill=(249,249,244),anchor="lm")
    plate.save(path,quality=95,subsampling=0)


def report():
    stats,checks={},{}
    columns=["gpu_frame_ms","gpu_camera_ms","gpu_shadow_ms","camera_triangles","shadow_triangles"]
    columns += [f"cascade{i}_{suffix}" for i in range(3) for suffix in ("triangles","gpu_ms")]
    for pose in POSES:
        stats[pose]={}
        for arm in ARMS:
            rows=[]
            for folder in (OUT/arm,OUT/arm/"repeat"):
                with (folder/(pose+".timings.csv")).open() as f:
                    rows.extend(csv.DictReader(f))
            stats[pose][arm]={k:median(float(r[k]) for r in rows) for k in columns}
        rgb=pose+".png"
        shadow=pose+"-shadow-visibility.png"
        checks[pose]={}
        for arm,name in [("before",rgb),("before",shadow),("sorted-unculled",rgb),
                         ("sorted-unculled",shadow),("camera-only-tiles",shadow),
                         ("camera-only-tufts",shadow),("no-oak-shadow",shadow)]:
            checks[pose][arm+"/"+name]=image_error(OUT/"culled"/name,OUT/arm/name)
        panels(OUT/(pose+"-comparison.jpg"),[
            ("Original",OUT/"before"/rgb),("Tile culling",OUT/"culled"/rgb),
            ("Tile culling + meadow revision",OUT/"meadow"/rgb)],920)
        crop=(2280,1000,3180,1490) if pose=="canopy" else (1850,580,2750,1070)
        panels(OUT/(pose+"-detail.jpg"),[("Original growth",OUT/"culled"/rgb),
                                         ("Revised growth",OUT/"meadow"/rgb)],900,crop=crop)
    checks["clearing"]["no-oak-camera/clearing.png"]=image_error(OUT/"culled/clearing.png",OUT/"no-oak-camera/clearing.png")
    panels(OUT/"offscreen-oak.jpg",[
        ("Normal shadows",OUT/"culled/clearing-shadow-visibility.png"),
        ("Oak shadow casting disabled",OUT/"no-oak-shadow/clearing-shadow-visibility.png")],810)
    # Show the part of the clearing where rejecting off-camera grass changes the most
    # shadow samples. Use identical raw crops without contrast amplification.
    shadow="clearing-shadow-visibility.png"
    a=np.asarray(Image.open(OUT/"culled"/shadow),dtype=np.int16)[:,:,0]
    b=np.asarray(Image.open(OUT/"camera-only-tufts"/shadow),dtype=np.int16)[:,:,0]
    difference=np.abs(a-b)
    h,w=a.shape
    cw,ch=480,320
    xs=sorted({*range(0,w-cw,80),w-cw})
    ys=sorted({*range(0,h-ch,80),h-ch})
    _,x,y=max((difference[y:y+ch,x:x+cw].sum(),x,y) for x in xs for y in ys)
    panels(OUT/"offscreen-grass.jpg",[
        ("Normal shadows",OUT/"culled"/shadow),
        ("Off-camera grass casters removed",OUT/"camera-only-tufts"/shadow)],560,crop=(x,y,x+cw,y+ch))
    (OUT/"measurements.json").write_text(json.dumps(dict(medians=stats,imageChecks=checks),indent=2))
    with (OUT/"timing-summary.csv").open("w",newline="") as f:
        writer=csv.DictWriter(f,fieldnames=["pose","variant",*columns])
        writer.writeheader()
        for pose,arms in stats.items():
            for arm,values in arms.items():
                writer.writerow(dict(pose=pose,variant=arm,**values))


if __name__=="__main__":
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--report-only",action="store_true")
    if not parser.parse_args().report_only:
        benchmark()
    report()
