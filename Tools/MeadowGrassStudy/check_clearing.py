"""Report paired clearing timings, registered native-scale crops and walk comparisons."""
import argparse
import tempfile
from pathlib import Path
from statistics import median

import numpy as np
from PIL import Image, ImageChops, ImageDraw, ImageFont

import clearing as study
from check_camera_lod import encode, panels, read, rgb, write

LABEL = {"reference":"Current LOD", "no-blades":"Tall blades hidden", "no-stems":"Stems and sheaths hidden",
         "no-heads":"Seed-head glumes hidden", "no-normal":"Blade normal map off",
         "no-glint":"Blade glint off", "no-surface":"Both off", "stems":"Lighter stems and sheaths"}
SURFACE = ("reference","no-normal","no-glint","no-surface")
FONT = ImageFont.truetype("C:/Windows/Fonts/segoeui.ttf",21)


def grid(images):
    w,h=images[0][1].size
    sheet=Image.new("RGB",(2*w,2*(h+38)),(28,32,28))
    draw=ImageDraw.Draw(sheet)
    for i,(label,im) in enumerate(images):
        x,y=i%2*w,i//2*(h+38)
        draw.text((x+10,y+4),label,font=FONT,fill="white")
        sheet.paste(im,(x,y+38))
    return sheet


def measure(trial):
    root=study.REVIEW
    prefix="trial" if trial else "run"
    arms=("reference","stems") if trial else [a for a in study.ARMS if a!="stems"]
    runs=4 if trial else 3
    report=[]
    for size in study.lod.bank.SIZES:
        first=root/(prefix+"1")/size
        base=rgb(first/"reference/clearing.png")
        previous=rgb(study.lod.REVIEW/"forward"/size/"lod/clearing.png")
        assert ImageChops.difference(base,previous).getbbox() is None
        counts=set()
        reference_triangles=None
        for arm in arms:
            im=rgb(first/arm/"clearing.png")
            difference=np.asarray(ImageChops.difference(base,im))
            row=dict(resolution=size,arm=arm,mean_abs_display_error=round(float(difference.mean()),4),
                     changed_over_8_percent=round(float((difference.max(2)>8).mean()*100),4))
            data=[]
            for run in range(1,runs+1):
                records=read(root/f"{prefix}{run}"/size/arm/"clearing.timings.csv")
                assert len(records)==(72 if trial else 12)
                data.append(records[36:] if trial else records)
                counts.update(tuple(r[f"cascade{i}_triangles"] for i in range(3)) for r in records)
            for field in ("gpu_camera_ms","gpu_frame_ms","gpu_shadow_ms","camera_triangles","camera_draws","cpu_ms"):
                values=[median(float(r[field]) for r in batch) for batch in data]
                row[field]=round(median(values),3)
                if field=="gpu_camera_ms":
                    for number,value in enumerate(values,1): row[f"run{number}_camera_ms"]=round(value,3)
            if arm=="reference": reference_triangles=row["camera_triangles"]
            if arm in SURFACE[1:]: assert row["camera_triangles"]==reference_triangles
            report.append(row)
        assert len(counts)==1
        if size=="4k":
            regions=(1160,780,1928,1300)
            def crops(arms,region=regions):
                return [(LABEL[a],rgb(first/a/"clearing.png").crop(region)) for a in arms]
            if trial:
                panels(root/"stems-detail.jpg",crops(arms),768)
                panels(root/"clearing-stems.jpg",[(LABEL[a],rgb(first/a/"clearing.png")) for a in arms],880)
            else:
                grid(crops(("reference","no-blades","no-stems","no-heads"))).save(root/"geometry.jpg",quality=95,subsampling=0)
                grid(crops(SURFACE,(510,1240,1278,1760))).save(root/"blade-shading.jpg",quality=96,subsampling=0)
    write(root/("stem-timings.csv" if trial else "ablation-timings.csv"),report)


def motion(trial):
    arms=("reference","stems") if trial else SURFACE
    title="stem-walk" if trial else "blade-shading-walk"
    root=study.REVIEW/"walk"
    manifests={a:read(root/a/"frames.csv") for a in arms}
    old={}
    rows=[]
    with tempfile.TemporaryDirectory(dir=root) as folder:
        folder=Path(folder)
        for i in range(study.lod.FRAMES):
            frames={a:rgb(root/a/f"frame-{i:06}.png") for a in arms}
            record=manifests["reference"][i]
            for a in arms[1:]:
                assert record["shadow_triangles"]==manifests[a][i]["shadow_triangles"]
                if not trial: assert record["camera_triangles"]==manifests[a][i]["camera_triangles"]
            ref=np.asarray(frames["reference"],dtype=np.int16)
            def crest(image):
                strip=image[340:980,320:2240]
                valid=(strip[:,:,1]-strip[:,:,2]>12).reshape(640,240,8).any(2)
                assert valid.any(axis=0).all()
                return valid.argmax(axis=0)
            baseline=crest(ref)
            for a in arms[1:]:
                other=np.asarray(frames[a],dtype=np.int16)
                difference=other-ref
                roi=difference[430:1200,320:2240]
                shift=np.abs(crest(other)-baseline)
                rows.append(dict(frame=i,arm=a,mean_abs_display_error=round(float(np.abs(difference).mean()),4),
                                 changed_over_8_percent=round(float((np.abs(difference).max(2)>8).mean()*100),4),
                                 ridge_within_one_pixel=int((shift<=1).sum()),ridge_strips=len(shift),
                                 temporal_residual=round(float(np.abs(roi-old[a]).mean()),4) if i else 0))
                old[a]=roi.copy()
            if trial:
                sheet=Image.new("RGB",(1600,742),(28,32,28))
                draw=ImageDraw.Draw(sheet)
                for col,a in enumerate(arms):
                    frame=frames[a]
                    sheet.paste(frame.resize((800,450),Image.Resampling.LANCZOS),(800*col,38))
                    sheet.paste(frame.crop((880,450,1680,664)),(800*col,528))
                    draw.text((800*col+12,4),LABEL[a],font=FONT)
                    draw.text((800*col+12,492),"Native pixels along the ridge",font=FONT)
            else:
                sheet=grid([(LABEL[a],frames[a].crop((190,810,990,1143))) for a in arms])
            sheet.save(folder/f"{i:03}.jpg",quality=95,subsampling=0)
        write(study.REVIEW/(title+".csv"),rows)
        encode(folder,study.REVIEW/(title+".mp4"))


def highlight_tracks():
    # Track grass texture with glint disabled so moving highlights cannot drive the match.
    # Score the brightening within a 5x5 neighbourhood on the independently matched blade.
    from PIL import ImageFilter
    root=study.REVIEW/"walk"
    rows=[]
    for i in range(4,study.lod.FRAMES-1,8):
        diffuse=[]
        glint=[]
        for frame in (i,i+1):
            def luminance(arm):
                with Image.open(root/arm/f"frame-{frame:06}.png") as im:
                    return np.asarray(im.convert("L"),dtype=np.int16)
            plain=luminance("no-glint")
            difference=np.maximum(0,luminance("reference")-plain).astype(np.uint8)
            diffuse.append(plain)
            glint.append(np.asarray(Image.fromarray(difference).filter(ImageFilter.BoxBlur(2))))
        for y0 in range(760,1230,120):
            for x0 in range(110,1440,180):
                cell=glint[0][y0:y0+110,x0:x0+170]
                dy,dx=np.unravel_index(cell.argmax(),cell.shape)
                y,x=y0+dy,x0+dx
                bright=int(glint[0][y,x])
                if bright<8: continue
                # A 17x17 patch searched up to 30 pixels from its previous position.
                patch=diffuse[0][y-8:y+9,x-8:x+9].astype(np.float32)
                patch-=patch.mean()
                length=np.sum(patch*patch)
                if length<17*17*4**2: continue
                neighbourhood=diffuse[1][y-38:y+39,x-38:x+39].astype(np.float32)
                windows=np.lib.stride_tricks.sliding_window_view(neighbourhood,(17,17))
                centered=windows-windows.mean(axis=(-2,-1),keepdims=True)
                correlation=(centered*patch).sum(axis=(-2,-1))/np.sqrt(np.maximum(1,length*(centered*centered).sum(axis=(-2,-1))))
                oy,ox=np.unravel_index(correlation.argmax(),correlation.shape)
                if min(oy,ox,60-oy,60-ox)==0 or correlation[oy,ox]<.82: continue
                ny,nx=y+oy-30,x+ox-30
                after=int(glint[1][ny-1:ny+2,nx-1:nx+2].max())
                rows.append(dict(frame=i,x=x,y=y,match=round(float(correlation[oy,ox]),3),
                                 highlight=bright,next_highlight=after,retained_half=int(2*after>=bright)))
    write(study.REVIEW/"highlight-tracks.csv",rows)


if __name__=="__main__":
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument("mode",choices=("timings","motion","highlights"))
    parser.add_argument("--trial",action="store_true")
    args=parser.parse_args()
    if args.mode=="highlights": highlight_tracks()
    else: (measure if args.mode=="timings" else motion)(args.trial)
