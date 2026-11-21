"""Validate camera LOD timings, paired pixels, silhouettes and the moving comparison."""
import csv
import json
import os
from pathlib import Path
from statistics import median
import subprocess
import tempfile

import numpy as np
from PIL import Image, ImageChops, ImageDraw, ImageFont

import camera_lod as study
from check_camera_meadow import panels, rgb


def read(path):
    with path.open() as stream:
        return list(csv.DictReader(stream))


def write(path, rows):
    with path.open("w",newline="") as stream:
        writer=csv.DictWriter(stream,fieldnames=rows[0])
        writer.writeheader()
        writer.writerows(rows)


def stills():
    review=study.REVIEW
    results=[]
    for pose in study.bank.far.CAMERAS:
        for size in study.bank.SIZES:
            root=review/"forward"/size
            full,mid=[rgb(root/arm/(pose+".png")) for arm in study.ARMS]
            reference=rgb(study.bank.REVIEW/"forward"/size/"all"/(pose+".png"))
            assert ImageChops.difference(full,reference).getbbox() is None
            delta=np.asarray(ImageChops.difference(mid,full))
            row=dict(pose=pose,resolution=size,mean_abs_display_error=round(float(delta.mean()),4),
                     pixels_above_8_percent=round(float((delta.max(2)>8).mean()*100),3))
            cascade_reference=None
            for arm in study.ARMS:
                series=[read(review/run/size/arm/(pose+".timings.csv")) for run in ("forward","reverse")]
                assert all(len(r)==12 for r in series)
                samples=series[0]+series[1]
                cascades={tuple(int(s[f"cascade{i}_triangles"]) for i in range(3)) for s in samples}
                assert len(cascades)==1
                if cascade_reference is None: cascade_reference=cascades
                assert cascades==cascade_reference
                for k in ("camera_triangles","camera_draws","gpu_camera_ms","gpu_frame_ms","gpu_shadow_ms","cpu_ms"):
                    row[arm+"_"+k]=round(median(float(s[k]) for s in samples),3)
                row[arm+"_run_medians_ms"]=" / ".join(f"{median(float(s['gpu_camera_ms']) for s in run):.3f}" for run in series)
            assert row["lod_camera_triangles"]<row["full_camera_triangles"]
            results.append(row)
            if size=="4k":
                panels(review/(pose+".jpg"),[("Full detail",full),("Camera LOD",mid)],960)
                if pose=="clearing":
                    crop=(1280,680,2176,1192)
                    panels(review/"skyline.jpg",[("Full detail",full.crop(crop)),("Camera LOD",mid.crop(crop))],896)
    write(review/"timings.csv",results)


def encode(folder, destination=None):
    encoder=folder/"encode.py"
    encoder.write_text("""import bpy
from pathlib import Path
p=Path(__file__).parent
files=sorted(p.glob('*.jpg'))
s=bpy.context.scene
editor=s.sequence_editor_create()
strip=editor.strips.new_image('Registered engine views',str(files[0]),channel=1,frame_start=1)
for image in files[1:]: strip.elements.append(image.name)
strip.frame_final_duration=len(files)
s.frame_end=len(files)
s.render.resolution_x,s.render.resolution_y=1600,742
s.render.resolution_percentage=100
s.render.fps=24
s.view_settings.view_transform='Standard'
s.view_settings.look='None'
s.render.image_settings.media_type='VIDEO'
s.render.image_settings.file_format='FFMPEG'
s.render.ffmpeg.format='MPEG4'
s.render.ffmpeg.codec='H264'
s.render.ffmpeg.constant_rate_factor='HIGH'
s.render.filepath=str(p/'walk.mp4')
bpy.ops.render.render(animation=True)
strip.mute=True
movie=editor.strips.new_movie('Decoded movie check',str(p/'walk.mp4'),channel=2,frame_start=1)
assert movie.frame_duration==len(files)
s.render.image_settings.media_type='IMAGE'
s.render.image_settings.file_format='PNG'
for i in (0,len(files)//2,len(files)-1):
    s.frame_set(i+1)
    s.render.filepath=str(p/f'decoded-{i:03}.png')
    bpy.ops.render.render(write_still=True)
""")
    blender=Path(os.environ["ProgramFiles"])/"Blender Foundation/Blender 5.0/blender.exe"
    with (folder/"encoding.log").open("w") as log:
        subprocess.run([blender,"-b","--factory-startup","--python-exit-code","1","-P",encoder],
                       check=True,stdout=log,stderr=subprocess.STDOUT)
    for i in (0,study.FRAMES//2,study.FRAMES-1):
        source=np.asarray(rgb(folder/f"{i:03}.jpg").resize((200,93)),dtype=np.int16)
        decoded=np.asarray(rgb(folder/f"decoded-{i:03}.png").resize((200,93)),dtype=np.int16)
        assert np.abs(source-decoded).mean()<5
    (folder/"walk.mp4").replace(destination or study.REVIEW/"walk.mp4")


def motion():
    root=study.REVIEW/"walk"
    manifests=[read(root/arm/"frames.csv") for arm in study.ARMS]
    assert all(len(manifest)==study.FRAMES for manifest in manifests)
    font=ImageFont.truetype("C:/Windows/Fonts/segoeui.ttf",21)
    records=[]
    previous=None
    with tempfile.TemporaryDirectory(dir=root) as tmp:
        tmp=Path(tmp)
        for i,(ref,mid) in enumerate(zip(*manifests)):
            assert ref["shadow_triangles"]==mid["shadow_triangles"]
            assert int(ref["camera_triangles"])>int(mid["camera_triangles"])
            sheet=Image.new("RGB",(1600,742),(28,32,28))
            draw=ImageDraw.Draw(sheet)
            frames=[]
            for col,(arm,label) in enumerate(zip(study.ARMS,("Full detail","Camera LOD"))):
                im=rgb(root/arm/f"frame-{i:06}.png")
                sheet.paste(im.resize((800,450),Image.Resampling.LANCZOS),(800*col,38))
                sheet.paste(im.crop((880,450,1680,664)),(800*col,528))
                draw.text((800*col+12,4),label,font=font)
                draw.text((800*col+12,492),"Native pixels along the ridge",font=font)
                frames.append(np.asarray(im,dtype=np.int16))
            delta=frames[1]-frames[0]
            absolute=np.abs(delta).max(2)
            # Fixed region containing grass and grass against sky on this registered path.
            roi=delta[430:1200,320:2240]
            temporal=0 if previous is None else np.abs(roi-previous).mean()
            records.append(dict(frame=i,mean_abs_display_error=round(float(np.abs(delta).mean()),4),
                                pixels_above_8_percent=round(float((absolute>8).mean()*100),3),
                                grass_temporal_residual=round(float(temporal),4),
                                full_triangles=ref["camera_triangles"],lod_triangles=mid["camera_triangles"]))
            previous=roi.copy()
            sheet.save(tmp/f"{i:03}.jpg",quality=94,subsampling=0)
        write(study.REVIEW/"walk-errors.csv",records)
        with (study.REVIEW/"walk-check.json").open("w") as f:
            json.dump({key:max(records,key=lambda r:r[key]) for key in
                      ("mean_abs_display_error","pixels_above_8_percent","grass_temporal_residual")},f,indent=2)
        encode(tmp)


def skyline():
    # Measure the visible crest in eight-pixel strips. A small green-over-blue
    # margin counts resolved grass while excluding the blue sky and white clouds.
    rows=[]
    for i in range(study.FRAMES):
        heights=[]
        for arm in study.ARMS:
            path=study.REVIEW/"walk"/arm/f"frame-{i:06}.png"
            with Image.open(path) as im:
                strip=np.asarray(im.crop((320,340,2240,980)).convert("RGB"),dtype=np.int16)
            grass=strip[:,:,1]-strip[:,:,2]>12
            bands=grass.reshape(640,240,8).any(axis=2)
            assert bands.any(axis=0).all()
            heights.append(np.argmax(bands,axis=0))
        shift=np.abs(heights[1]-heights[0])
        rows.append(dict(frame=i,within_one_pixel=int((shift<=1).sum()),strips=len(shift),
                         largest_vertical_shift_px=int(shift.max())))
    write(study.REVIEW/"skyline.csv",rows)


if __name__=="__main__":
    stills()
    motion()
    skyline()
