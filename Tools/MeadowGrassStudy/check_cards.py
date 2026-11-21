"""Check paired shadow submissions and generate the density, ridge and walk comparisons."""
import argparse
from statistics import median
import tempfile
from pathlib import Path

import numpy as np
from PIL import Image, ImageChops, ImageDraw, ImageFont
import cards
from check_camera_lod import read, write, rgb, panels, encode


def cascades(sample):
    return tuple(sample[f"cascade{i}_triangles"] for i in range(3))


def oak():
    root=cards.REVIEW
    rows=[]
    for pose in cards.GROVE.CAMERAS:
        for run in ("oak1","oak2"):
            files=[root/run/"4k"/arm/pose for arm in ("unculled","geometry")]
            a,b=[rgb(path.with_suffix(".png")) for path in files]
            assert ImageChops.difference(a,b).getbbox() is None
            old,new=[read(path.with_suffix(".timings.csv"))[12:] for path in files]
            assert all(cascades(p)==cascades(q) for p,q in zip(old,new))
            rows.append(dict(view=pose,run=run,triangles_saved=int(old[0]["camera_triangles"])-int(new[0]["camera_triangles"]),
                             old_gpu_ms=median(float(p["gpu_frame_ms"]) for p in old),
                             new_gpu_ms=median(float(q["gpu_frame_ms"]) for q in new)))
    write(root/"oak-culling.csv",rows)


def crest_coverage(images):
    # The same 1,840-pixel-wide section of the clearing in every 4K capture.
    # Anchor a 140-pixel band at the first green grass pixel per 8 columns of the geometry reference.
    masks=[]
    for im in images:
        strip=np.asarray(im.crop((1000,480,2840,1500)),dtype=np.int16)
        masks.append(strip[:,:,1]-strip[:,:,2]>12)
    top=masks[1].reshape(1020,-1,8).any(2).argmax(0).repeat(8)
    y=top[None,:]+np.arange(140)[:,None]
    return [dict(arm=arm,green_percent=100*float(mask[y,np.arange(1840)].mean()))
            for arm,mask in zip(("previous-thinned","dense-geometry","dense-cards"),masks)]


def shadow_check():
    root=cards.REVIEW
    live=root/"forward/4k/cards"
    disabled=root/"final-no-oak-shadow/4k/cards"
    bare=root/"final-no-grass/4k/cards"
    frames=[read(folder/"clearing.timings.csv")[0] for folder in (live,disabled,bare)]
    assert frames[0]["camera_triangles"]==frames[1]["camera_triangles"]
    assert int(frames[0]["shadow_triangles"])>0 and int(frames[1]["shadow_triangles"])==0
    assert cascades(frames[0])==cascades(frames[2])
    with_oaks,no_oaks=[rgb(folder/"clearing.png") for folder in (live,disabled)]
    delta=np.asarray(ImageChops.difference(with_oaks,no_oaks))
    assert (delta.max(2)>8).any()
    print("Offscreen oak shadows change",int((delta.max(2)>8).sum()),"4K pixels by more than 8/255")
    panels(root/"offscreen-oak-shadow.jpg",[("Offscreen oak shadows",with_oaks),
                                            ("Oak casters disabled",no_oaks)],900)
    control=rgb(root/"final-oak-control/4k/cards/downhill.png")
    assert ImageChops.difference(control,rgb(live/"downhill.png")).getbbox() is None


def stills():
    root=cards.REVIEW
    records=[]
    for pose in cards.GROVE.CAMERAS:
        for size in ("4k","1440p"):
            reference=None
            for arm in ("geometry","cards"):
                series=[read(root/run/size/arm/(pose+".timings.csv")) for run in ("forward","reverse")]
                assert all(len(s)==36 for s in series)
                samples=series[0][12:]+series[1][12:]
                shadow={cascades(s) for s in samples}
                assert len(shadow)==1 and (reference is None or reference==shadow)
                reference=shadow
                row=dict(pose=pose,size=size,arm=arm)
                for key in ("camera_triangles","gpu_camera_ms","gpu_shadow_ms","gpu_frame_ms","fenced_frame_ms","cpu_ms"):
                    row[key]=round(median(float(s[key]) for s in samples),3)
                records.append(row)
        old=rgb(cards.sparse.REVIEW/"sward1/4k/sward-contact"/(pose+".png"))
        geo,card=[rgb(root/"forward/4k"/arm/(pose+".png")) for arm in ("geometry","cards")]
        panels(root/(pose+"-density.jpg"),[("Previous thinned meadow",old),("Denser meadow with cards",card)],960)
        panels(root/(pose+"-paired.jpg"),[("Dense geometry",geo),("Same plants with cards",card)],960)
        if pose=="clearing":
            write(root/"crest-coverage.csv",crest_coverage((old,geo,card)))
            box=(1400,720,2400,1180)
            panels(root/"ridge.jpg",[("Dense geometry",geo.crop(box)),("Same plants with cards",card.crop(box))],1000)
    write(root/"timings.csv",records)


def motion():
    root=cards.REVIEW/"walk"
    arms=("geometry","cards")
    series=[read(root/arm/"frames.csv") for arm in arms]
    assert all(len(s)==97 for s in series)
    records=[]
    font=ImageFont.truetype("C:/Windows/Fonts/segoeui.ttf",21)
    previous=None
    with tempfile.TemporaryDirectory(dir=root) as folder:
        folder=Path(folder)
        for frame,(g,c) in enumerate(zip(*series)):
            assert g["shadow_triangles"]==c["shadow_triangles"] and int(g["camera_triangles"])>int(c["camera_triangles"])
            sheet=Image.new("RGB",(1600,742),(28,32,28))
            draw=ImageDraw.Draw(sheet)
            heights=[]
            colors=[]
            for col,(arm,title) in enumerate(zip(arms,("Dense geometry","Same plants with cards"))):
                im=rgb(root/arm/f"frame-{frame:06}.png")
                sheet.paste(im.resize((800,450),Image.Resampling.LANCZOS),(col*800,38))
                sheet.paste(im.crop((880,450,1680,664)),(col*800,528))
                draw.text((col*800+12,4),title,font=font)
                draw.text((col*800+12,492),"Ridge at native resolution",font=font)
                strip=np.asarray(im.crop((320,340,2240,980)),dtype=np.int16)
                bands=(strip[:,:,1]-strip[:,:,2]>12).reshape(640,240,8).any(axis=2)
                assert bands.any(axis=0).all()
                heights.append(np.argmax(bands,axis=0))
                colors.append(strip)
            shift=heights[1]-heights[0]
            delta=colors[1]-colors[0]
            records.append(dict(frame=frame,crest_median_shift_px=float(np.median(shift)),
                crest_abs_p95_px=float(np.percentile(np.abs(shift),95)),ridge_color_error=float(np.abs(delta).mean()),
                ridge_temporal_error=float(np.abs(delta-previous).mean()) if previous is not None else 0))
            previous=delta
            sheet.save(folder/f"{frame:03}.jpg",quality=94,subsampling=0)
        write(root/"ridge.csv",records)
        encode(folder,cards.REVIEW/"grass-walk.mp4")


if __name__=="__main__":
    ap=argparse.ArgumentParser()
    modes={"stills":stills,"motion":motion,"oak":oak,"shadows":shadow_check}
    ap.add_argument("mode",choices=modes)
    modes[ap.parse_args().mode]()
