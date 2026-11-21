"""Validate the sparse camera and caster controls, then assemble registered comparisons."""
import argparse
from pathlib import Path
from statistics import median
import tempfile

import numpy as np
from PIL import Image,ImageChops,ImageDraw,ImageFont

import sparse as study
from check_camera_lod import encode,panels,read,rgb,write

ROOT=study.REVIEW
NAMES={"dense":"Previous grass", "sparse":"Fewer, wider blades", "sparse-contact":"Baked ground contact",
       "patch-contact":"Fewer tufts + ground contact", "patch-near":"Fewer tufts + near grass shadows"}


def summary():
    times=[]
    for group,runs in (("blades",("forward","reverse")),("patch",("trial1","trial2"))):
        for first in sorted((ROOT/runs[0]).glob("*/*/*.timings.csv")):
            name=first.relative_to(ROOT/runs[0])
            size,arm=name.parts[:2]
            pose=first.name.split(".")[0]
            row={"test":group,"view":pose,"size":size,"arm":arm}
            records=[read(ROOT/run/name) for run in runs]
            assert all(len(batch)==36 for batch in records)
            for key in ("camera_triangles","camera_draws",*(f"cascade{i}_triangles" for i in range(3))):
                counters={int(frame[key]) for batch in records for frame in batch}
                assert len(counters)==1
                row[key]=counters.pop()
            for key in ("gpu_camera_ms","gpu_shadow_ms","gpu_frame_ms","cpu_ms"):
                medians=[median(float(s[key]) for s in batch[12:]) for batch in records]
                row[key]=round(median(medians),3)
                if key=="gpu_frame_ms": row["repeat_frame_ms"]=" / ".join(f"{v:.3f}" for v in medians)
            times.append(row)
    write(ROOT/"results.csv",times)
    for pose in study.GROVE.CAMERAS:
        old=ROOT/"forward/4k"
        new=ROOT/"trial1/4k"
        path=pose+".png"
        images={a:rgb((old if a in ("dense","sparse","sparse-contact") else new)/a/path) for a in NAMES}
        assert ImageChops.difference(images["dense"],rgb(new/"dense"/path)).getbbox() is None
        # The second shader expresses the same reciprocal width in a different operation order.
        crossbuild=np.asarray(ImageChops.difference(images["sparse-contact"],rgb(new/"sparse-contact"/path)))
        assert np.count_nonzero(crossbuild.max(2))<100
        reference,blades,contact=[next(r for r in times if r["test"]=="blades" and r["size"]=="4k"
              and r["view"]==pose and r["arm"]==a) for a in ("dense","sparse","sparse-contact")]
        assert blades["camera_triangles"]==contact["camera_triangles"]<reference["camera_triangles"]
        assert all(reference[f"cascade{i}_triangles"]==blades[f"cascade{i}_triangles"] for i in range(3))
        assert all(contact[f"cascade{i}_triangles"]<blades[f"cascade{i}_triangles"] for i in range(3))
        patch,near=[next(r for r in times if r["test"]=="patch" and r["size"]=="4k"
              and r["view"]==pose and r["arm"]==a) for a in ("patch-contact","patch-near")]
        assert near["camera_triangles"]==patch["camera_triangles"]
        assert near["cascade0_triangles"]==blades["cascade0_triangles"]
        for i in (1,2): assert near[f"cascade{i}_triangles"]==contact[f"cascade{i}_triangles"]
        panels(ROOT/(pose+"-density.jpg"),[(NAMES[a],images[a]) for a in ("dense","sparse")],880)
        panels(ROOT/(pose+"-patch.jpg"),[(NAMES[a],images[a]) for a in ("sparse-contact","patch-contact")],880)
        panels(ROOT/(pose+"-casters.jpg"),[(label,images[a]) for a,label in
               (("sparse","Baked contact + grass sun shadows"),("sparse-contact","Baked contact only for grass"))],880)
    # Camera-hidden soil makes the two different contact contributions legible.
    controls=[("Grass shadows + ground bake","soil","sparse"),("Ground bake","soil","sparse-contact"),
              ("Grass shadows and bake off","soil-no-bake","sparse-contact")]
    soil=[(label,rgb(ROOT/run/"4k"/arm/"canopy.png")) for label,run,arm in controls]
    assert ImageChops.difference(soil[1][1],soil[2][1]).getbbox()
    panels(ROOT/"soil.jpg",soil,600)
    soilcounts=[read(ROOT/run/"4k"/arm/"canopy.timings.csv")[0] for _,run,arm in controls]
    assert soilcounts[1]["shadow_triangles"]==soilcounts[2]["shadow_triangles"]
    for s in soilcounts: assert s["camera_triangles"]==soilcounts[0]["camera_triangles"]


def motion():
    font=ImageFont.truetype("C:/Windows/Fonts/segoeui.ttf",20)
    arms=("dense","sparse","sparse-contact","patch-contact","patch-near")
    manifest={a:read(ROOT/"walk"/a/"frames.csv") for a in arms}
    assert all(len(m)==study.previous.lod.FRAMES for m in manifest.values())
    rows=[]
    with tempfile.TemporaryDirectory(dir=ROOT/"walk") as folder:
        folder=Path(folder)
        for i in range(study.previous.lod.FRAMES):
            m=[manifest[a][i] for a in arms]
            assert m[0]["shadow_triangles"]==m[1]["shadow_triangles"]
            assert m[1]["camera_triangles"]==m[2]["camera_triangles"]
            assert m[3]["camera_triangles"]==m[4]["camera_triangles"]
            sheet=Image.new("RGB",(1600,742),(29,33,28))
            draw=ImageDraw.Draw(sheet)
            crests={}
            for a in arms:
                im=rgb(ROOT/"walk"/a/f"frame-{i:06}.png")
                strip=np.asarray(im.crop((320,340,2240,980)),dtype=np.int16)
                bins=(strip[:,:,1]-strip[:,:,2]>12).reshape(640,240,8).any(2)
                assert bins.any(0).all()
                crests[a]=bins.argmax(0)
                if a in ("dense","sparse","sparse-contact","patch-contact"):
                    idx=arms.index(a)
                    x,y=(idx%2)*800,(idx//2)*371
                    draw.text((x+12,y+5),NAMES[a],font=font)
                    # Same horizontal field and walking path. Only sky and lower margin are cropped.
                    sheet.paste(im.crop((0,245,2560,1310)).resize((800,333),Image.Resampling.LANCZOS),(x,y+38))
            for a,b in (("dense","sparse"),("sparse-contact","patch-contact")):
                shift=np.abs(crests[a]-crests[b])
                rows.append(dict(frame=i,baseline=a,candidate=b,within_one_pixel=int((shift<=1).sum()),
                                 strips=len(shift),median_shift=median(shift),p95_shift=float(np.percentile(shift,95))))
            sheet.save(folder/f"{i:03}.jpg",quality=95,subsampling=0)
        write(ROOT/"walk-crest.csv",rows)
        encode(folder,ROOT/"meadow-walk.mp4")


def final_review():
    # Merge the spatially distributed tuft trial with the earlier isolated tests.
    earlier=read(ROOT/"results.csv")
    earlier=[r for r in earlier if r["test"]!="sward"]
    rows=[]
    for file in sorted((ROOT/"sward1").glob("*/*/*.timings.csv")):
        name=file.relative_to(ROOT/"sward1")
        size,arm=name.parts[:2]
        pose=file.name.split(".")[0]
        batches=[read(ROOT/run/name) for run in ("sward1","sward2")]
        assert all(len(batch)==36 for batch in batches)
        row=dict(test="sward",view=pose,size=size,arm=arm)
        for key in ("camera_triangles","camera_draws",*(f"cascade{i}_triangles" for i in range(3))):
            counts={int(r[key]) for batch in batches for r in batch}
            assert len(counts)==1
            row[key]=counts.pop()
        for key in ("gpu_camera_ms","gpu_shadow_ms","gpu_frame_ms","cpu_ms"):
            times=[median(float(r[key]) for r in batch[12:]) for batch in batches]
            row[key]=round(median(times),3)
            if key=="gpu_frame_ms": row["repeat_frame_ms"]=" / ".join(f"{t:.3f}" for t in times)
        rows.append(row)
    write(ROOT/"results.csv",earlier+rows)
    for pose in study.GROVE.CAMERAS:
        path=pose+".png"
        root=ROOT/"sward1/4k"
        dense=rgb(root/"dense"/path)
        assert ImageChops.difference(dense,rgb(ROOT/"forward/4k/dense"/path)).getbbox() is None
        contact,near=[rgb(root/a/path) for a in ("sward-contact","sward-near")]
        rows4={r["arm"]:r for r in rows if r["view"]==pose and r["size"]=="4k"}
        assert rows4["sward-contact"]["camera_triangles"]==rows4["sward-near"]["camera_triangles"]
        assert rows4["sward-near"]["cascade0_triangles"]==rows4["dense"]["cascade0_triangles"]
        for i in (1,2): assert rows4["sward-contact"][f"cascade{i}_triangles"]==rows4["sward-near"][f"cascade{i}_triangles"]
        panels(ROOT/(pose+"-final.jpg"),[("Previous meadow",dense),("Distributed tufts + baked ground contact",contact)],880)
        panels(ROOT/(pose+"-shadow-choice.jpg"),[("Ground contact only for grass",contact),("Ground contact + near grass shadows",near)],880)
    reference=read(ROOT/"walk/dense/frames.csv")
    candidate=read(ROOT/"walk/sward-near/frames.csv")
    contact=read(ROOT/"walk/sward-contact/frames.csv")
    assert len(reference)==len(candidate)==len(contact)==study.previous.lod.FRAMES
    font=ImageFont.truetype("C:/Windows/Fonts/segoeui.ttf",20)
    motion_stats=[]
    with tempfile.TemporaryDirectory(dir=ROOT/"walk") as folder:
        folder=Path(folder)
        for i in range(len(reference)):
            assert candidate[i]["camera_triangles"]==contact[i]["camera_triangles"]
            sheet=Image.new("RGB",(1600,742),(29,33,28))
            draw=ImageDraw.Draw(sheet)
            crest=[]
            for k,(arm,label) in enumerate((("dense","Previous meadow"),("sward-contact","Distributed tufts + baked ground contact"))):
                im=rgb(ROOT/"walk"/arm/f"frame-{i:06}.png")
                sheet.paste(im.resize((800,450),Image.Resampling.LANCZOS),(800*k,38))
                sheet.paste(im.crop((880,450,1680,664)),(800*k,528))
                draw.text((800*k+10,5),label,font=font)
                draw.text((800*k+10,493),"Native pixels along the ridge",font=font)
                strip=np.asarray(im.crop((320,340,2240,980)),dtype=np.int16)
                bins=(strip[:,:,1]-strip[:,:,2]>12).reshape(640,240,8).any(2)
                assert bins.any(0).all()
                crest.append(bins.argmax(0))
            shifts=np.abs(crest[0]-crest[1])
            motion_stats.append(dict(frame=i,within_one_pixel=int((shifts<=1).sum()),strips=len(shifts),
                                      median_shift=median(shifts),p95_shift=float(np.percentile(shifts,95))))
            sheet.save(folder/f"{i:03}.jpg",quality=95,subsampling=0)
        write(ROOT/"distributed-walk.csv",motion_stats)
        encode(folder,ROOT/"distributed-walk.mp4")

if __name__=="__main__":
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument("mode",choices=("summary","motion","final"))
    {"summary":summary,"motion":motion,"final":final_review}[parser.parse_args().mode]()
