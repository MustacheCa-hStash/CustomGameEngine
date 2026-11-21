"""Repeatable captures, caster ablations and a paired walk across the far-shadow blend."""
import argparse
import csv
import json
import math
import os
from pathlib import Path
from statistics import median
import subprocess
import tempfile

import numpy as np
from PIL import Image, ImageDraw, ImageFont

import far_shadow as study
from check_grass_tiles import image_error, panels


REVIEW=study.OUT/"review"
MOTION=REVIEW/"walk"
FRAMES=85
FPS=24


def sequence(arm,lighting,reverse):
    # One world-space receiver remains at frame centre as its depth passes through
    # 12 m and 10 m. Endpoint eyes are 1.65 m above the actual terrain.
    x,z,yaw=-32,-13,150
    target=np.array((x,study.grove.height(x,z)+.12,z))
    endpoints=[]
    for horizontal in (15.5,8.5):
        ex=x+horizontal*math.sin(math.radians(yaw))
        ez=z+horizontal*math.cos(math.radians(yaw))
        rise=study.grove.height(ex,ez)+study.grove.EYE_HEIGHT-target[1]
        endpoints.append((math.hypot(horizontal,rise),math.degrees(math.atan2(rise,horizontal))))
    if reverse: endpoints.reverse()
    (start,pitch),(end,end_pitch)=endpoints
    name="out" if reverse else "in"
    parent=MOTION/arm/lighting
    parent.mkdir(exist_ok=True,parents=True)
    dest=parent/name
    if (dest/"frames.csv").exists() and len(list(dest.glob("frame-*.png")))==FRAMES:
        return
    focus=json.loads((study.STAGE/"scene.json").read_text())["focus"]
    with tempfile.TemporaryDirectory(dir=parent) as tmp:
        out=Path(tmp)/"frames"
        args=[study.STAGE/"FarGrass.exe","--capture",out,"--scene","forked-oak","--camera","overview",
              "--msaa","4","--width","1280","--height","720","--frames",str(FRAMES),"--fps",str(FPS),
              "--distance",str(start),"--end-distance",str(end),"--yaw-degrees",str(yaw),
              "--pitch-degrees",str(pitch),"--end-pitch-degrees",str(end_pitch),
              "--sun-azimuth","106","--sun-elevation","37","--exposure-ev","0.65",
              "--wind-strength","0","--wind-time","0","--probe-gi","bounce","--lighting",lighting]
        for axis,offset in zip("xyz",target-focus):
            args.extend(["--focus-offset-"+axis,str(offset)])
        subprocess.run(args,cwd=study.STAGE,env=study.environment(arm),check=True,timeout=360)
        out.replace(dest)


def measure():
    for direction in (False,True):
        for lighting in ("combined","shadow-visibility"):
            for arm in ("ground","both"):
                sequence(arm,lighting,direction)
    for name,arm,flag in [
        ("full","ground",None),("candidate","both",None),
        ("all-tiles","both","MEADOW_TEST_KEEP_ALL_TILES"),
        ("camera-grass","both","MEADOW_TEST_CAMERA_TUFTS"),
        ("far-camera-grass","both","MEADOW_TEST_FAR_CAMERA_TUFTS"),
        ("no-oak-shadow","both","MEADOW_TEST_NO_OAK_SHADOW"),
    ]:
        for pose in study.CAMERAS:
            study.capture(arm,pose,lighting="shadow-visibility",out=REVIEW/"checks"/name,check=flag)
    study.capture("both","clearing",out=REVIEW/"checks/no-oak-camera",check="MEADOW_TEST_NO_OAK_CAMERA")
    for repeat in (False,True):
        for pose in study.CAMERAS:
            for arm in (reversed(study.ARMS) if repeat else study.ARMS):
                dest=REVIEW/arm
                study.capture(arm,pose,out=dest/"repeat" if repeat else dest)


def motion_comparison():
    summaries={}
    font=ImageFont.truetype("C:/Windows/Fonts/segoeui.ttf",19)
    with tempfile.TemporaryDirectory(dir=REVIEW) as tmp:
        tmp=Path(tmp)
        for reverse,direction in enumerate(("in","out")):
            distances=list(csv.DictReader((MOTION/"both/combined"/direction/"frames.csv").open()))
            rows=[]
            previous=None
            for i,row in enumerate(distances):
                name=f"frame-{i:06}.png"
                depth=float(row["distance"])
                plate=Image.new("RGB",(1280,628),(24,29,25))
                draw=ImageDraw.Draw(plate)
                values={}
                for col,arm in enumerate(("ground","both")):
                    for layer in ("combined","shadow-visibility"):
                        with Image.open(MOTION/arm/layer/direction/name) as im:
                            if layer=="combined":
                                plate.paste(im.resize((640,360),Image.Resampling.LANCZOS),(col*640,36))
                            else:
                                plate.paste(im.crop((320,264,960,456)),(col*640,436))
                            values[arm,layer]=np.asarray(im.crop((545,310,735,410)),dtype=np.int16)
                draw.text((12,6),"Full far-shadow mesh",font=font)
                draw.text((652,6),"Simplified far-shadow mesh",font=font)
                draw.text((12,402),f"Shadow visibility at native scale     |     Target depth {depth:.2f} m     |     "+
                          ("Walking away" if reverse else "Walking toward the patch")+
                          ("     |     Cascade blend" if 10<=depth<=12 else ""),font=font)
                color=values["both","combined"]-values["ground","combined"]
                mask=values["both","shadow-visibility"][:,:,0]-values["ground","shadow-visibility"][:,:,0]
                frame=dict(depth=depth,displayMAE=float(np.abs(color).mean()),displayBias=float(color.mean()),
                           visibilityMAE=float(np.abs(mask).mean()),visibilityBias=float(mask.mean()),
                           displayTemporalBias=0 if previous is None else float(abs(color.mean()-previous)))
                previous=color.mean()
                rows.append(frame)
                plate.save(tmp/f"{reverse*FRAMES+i:04}.jpg",quality=95,subsampling=0)
            with (MOTION/(direction+".csv")).open("w",newline="") as f:
                writer=csv.DictWriter(f,fieldnames=list(rows[0]))
                writer.writeheader()
                writer.writerows(rows)
            blend=[r for r in rows if 10<=r["depth"]<=12]
            summaries[direction]=dict(frames=len(rows),blendFrames=len(blend),depths=[rows[0]["depth"],rows[-1]["depth"]],
                 maxBlendFrameMeanDisplayError=max(r["displayMAE"] for r in blend),
                 maxBlendTemporalBias=max(r["displayTemporalBias"] for r in blend),
                 maxBlendVisibilityBias=max(abs(r["visibilityBias"]) for r in blend))
        # Use Blender's bundled FFmpeg encoder. Colour inputs are display-referred
        # engine frames, so use Standard/sRGB with no additional look or exposure.
        script=tmp/"encode.py"
        script.write_text("""import bpy
from pathlib import Path
p=Path(__file__).parent
files=sorted(p.glob('*.jpg'))
s=bpy.context.scene
editor=s.sequence_editor_create()
strip=editor.strips.new_image('Engine comparison',str(files[0]),channel=1,frame_start=1)
for image in files[1:]: strip.elements.append(image.name)
strip.frame_final_duration=len(files)
s.frame_end=len(files)
s.render.resolution_x,s.render.resolution_y=1280,628
s.render.resolution_percentage=100
s.render.fps=24
s.view_settings.view_transform='Standard'
s.view_settings.look='None'
s.render.image_settings.media_type='VIDEO'
s.render.image_settings.file_format='FFMPEG'
s.render.ffmpeg.format='MPEG4'
s.render.ffmpeg.codec='H264'
s.render.ffmpeg.constant_rate_factor='HIGH'
s.render.filepath=str(p/'crossing.mp4')
bpy.ops.render.render(animation=True)
strip.mute=True
movie=editor.strips.new_movie('Encoded check',str(next(p.glob('*.mp4'))),channel=2,frame_start=1)
assert movie.frame_duration==len(files), (movie.frame_duration,len(files))
s.render.image_settings.media_type='IMAGE'
s.render.image_settings.file_format='PNG'
for frame in (1,43,85,128,170):
    s.frame_set(frame)
    s.render.filepath=str(p/f'decoded-{frame-1:04}.png')
    bpy.ops.render.render(write_still=True)
""")
        blender=Path(os.environ["ProgramFiles"])/"Blender Foundation/Blender 5.0/blender.exe"
        with (MOTION/"encode.log").open("w") as log:
            subprocess.run([blender,"-b","--factory-startup","--python-exit-code","1","-P",script],
                           stdout=log,stderr=subprocess.STDOUT,check=True)
        decode_error=[]
        for frame in (0,42,84,127,169):
            a=np.asarray(Image.open(tmp/f"{frame:04}.jpg").resize((160,79)),dtype=np.int16)
            b=np.asarray(Image.open(tmp/f"decoded-{frame:04}.png").convert("RGB").resize((160,79)),dtype=np.int16)
            decode_error.append(float(np.abs(a-b).mean()))
        if max(decode_error)>6:
            raise RuntimeError(f"Encoded motion frames do not match the source: {decode_error}")
        summaries["encodedVideo"]=dict(frames=2*FRAMES,fps=FPS,worstSampleMeanDecodeError=max(decode_error))
        next(tmp.glob("*.mp4")).replace(REVIEW/"cascade-crossing.mp4")
    return summaries


def report():
    keys=["gpu_frame_ms","gpu_camera_ms","gpu_shadow_ms","camera_triangles","shadow_triangles",
          "cascade0_triangles","cascade1_triangles","cascade2_triangles","cascade2_gpu_ms"]
    counts={}
    checks={}
    for pose in study.CAMERAS:
        counts[pose]={}
        for arm in study.ARMS:
            records=[]
            for run in [REVIEW/arm,REVIEW/arm/"repeat"]:
                with (run/(pose+".timings.csv")).open() as f:
                    records.extend(csv.DictReader(f))
            counts[pose][arm]={k:median(float(r[k]) for r in records) for k in keys}
            counts[pose][arm]["samples"]=len(records)
        name=pose+"-shadow-visibility.png"
        checks[pose]={k:image_error(REVIEW/"checks/candidate"/name,REVIEW/"checks"/k/name)
                      for k in ("full","all-tiles","camera-grass","far-camera-grass","no-oak-shadow")}
        name=pose+".png"
        panels(REVIEW/(pose+"-floor.jpg"),[("A   Soil and thatch",REVIEW/"shadow"/name),
                                           ("B   Soil, thatch and living cover",REVIEW/"both"/name)],960)
    checks["clearing"]["no-oak-camera"]=image_error(REVIEW/"both/clearing.png",REVIEW/"checks/no-oak-camera/clearing.png")
    panels(REVIEW/"ground-detail.jpg",[
        ("A   Soil and thatch",REVIEW/"shadow/clearing.png"),
        ("B   Living cover between tufts",REVIEW/"both/clearing.png")],840,crop=(1910,1310,2750,1770))
    panels(REVIEW/"offscreen-oak.jpg",[
        ("Normal shadows",REVIEW/"checks/candidate/clearing-shadow-visibility.png"),
        ("Oak shadows disabled",REVIEW/"checks/no-oak-shadow/clearing-shadow-visibility.png")],750)
    with (REVIEW/"timings.csv").open("w",newline="") as f:
        writer=csv.DictWriter(f,fieldnames=["pose","arm",*keys,"samples"])
        writer.writeheader()
        for pose,arms in counts.items():
            for arm,values in arms.items():
                writer.writerow(dict(pose=pose,arm=arm,**values))
    (REVIEW/"checks.json").write_text(json.dumps(dict(shadow=checks,motion=motion_comparison()),indent=2))


if __name__=="__main__":
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--report-only",action="store_true")
    if not parser.parse_args().report_only:
        measure()
    report()
