from pathlib import Path
from PIL import Image,ImageDraw,ImageFont
import numpy as np
from statistics import median
from check_camera_lod import read,write,rgb,panels,encode
import distant as d
import argparse,tempfile
R=d.OUT/'review'
def stills():
    records=[]
    for pose in ('canopy','clearing','downhill'):
        shadow=set()
        med={}
        for arm in ('near','bare','reference','optimized'):
            runs=[read(R/run/arm/(pose+'.timings.csv'))[12:] for run in ('forward','reverse')]
            assert all(len(run)==24 for run in runs)
            rows=runs[0]+runs[1]
            shadow.update(tuple(row[f'cascade{i}_triangles'] for i in range(3)) for row in rows)
            # Give each reversed-order run equal weight even if GPU clocks differ.
            med[arm]={k:sum(median(float(row[k]) for row in run) for run in runs)/2 for k in
                      ('gpu_camera_ms','gpu_frame_ms','camera_triangles','camera_draws','gpu_shadow_ms')}
        assert len(shadow)==1
        a,b=[rgb(R/'forward'/arm/(pose+'.png')) for arm in ('reference','optimized')]
        diff=np.abs(np.asarray(a,dtype=np.int16)-np.asarray(b,dtype=np.int16))
        for arm in med:
            row=dict(view=pose,arm=arm,**med[arm])
            row.update(far_increment_ms=row['gpu_camera_ms']-med['near']['gpu_camera_ms'] if arm!='bare' else '',
                       grass_increment_ms=row['gpu_camera_ms']-med['bare']['gpu_camera_ms'])
            if arm=='optimized':
                row.update(image_mean_abs_8bit=diff.mean(),pixels_over_8_percent=(diff.max(2)>8).mean()*100)
            records.append(row)
        panels(d.OUT/(pose+'.jpg'),[('Ribbon meadow reference',a),('Optimized distant grass',b)],850)
        if pose=='clearing':
            crop=(1420,735,2400,1075)
            panels(d.OUT/'ridge.jpg',[('Reference ridge',a.crop(crop)),('Optimized ridge',b.crop(crop))],980)
    for row in records:
        row.setdefault('image_mean_abs_8bit','')
        row.setdefault('pixels_over_8_percent','')
    write(d.OUT/'timings.csv',records)
    for r in records:
        print(r['view'],r['arm'],*[round(r[k],3) if r[k]!='' else '' for k in ('far_increment_ms','grass_increment_ms','gpu_frame_ms')])
def motion():
    root=R/'walk'
    arms=('reference','optimized')
    series=[read(root/arm/'frames.csv') for arm in arms]
    assert all(len(s)==97 for s in series)
    font=ImageFont.truetype('C:/Windows/Fonts/segoeui.ttf',21)
    rows=[]; previous=None
    with tempfile.TemporaryDirectory(dir=root) as folder:
        folder=Path(folder)
        for frame,(r1,r2) in enumerate(zip(*series)):
            assert r1['shadow_triangles']==r2['shadow_triangles']
            sheet=Image.new('RGB',(1600,742),(28,32,28))
            draw=ImageDraw.Draw(sheet)
            bands=[]; colors=[]
            for col,(arm,label) in enumerate(zip(arms,('Reference','Optimized distant grass'))):
                im=rgb(root/arm/f'frame-{frame:06}.png')
                sheet.paste(im.resize((800,450),Image.Resampling.LANCZOS),(800*col,38))
                sheet.paste(im.crop((880,450,1680,664)),(800*col,528))
                draw.text((800*col+12,4),label,font=font)
                draw.text((800*col+12,492),'Ridge at capture resolution',font=font)
                strip=np.asarray(im.crop((320,340,2240,980)),dtype=np.int16)
                mask=(strip[:,:,1]-strip[:,:,2]>12).reshape(640,240,8).any(2)
                assert mask.any(0).all()
                bands.append(mask.argmax(0))
                colors.append(strip)
            delta=colors[1]-colors[0]
            shift=np.abs(bands[1]-bands[0])
            rows.append(dict(frame=frame,crest_abs_p95_px=np.percentile(shift,95),
                crest_abs_max_px=shift.max(),ridge_abs_8bit=np.abs(delta).mean(),
                ridge_temporal_8bit=np.abs(delta-previous).mean() if previous is not None else 0))
            previous=delta
            sheet.save(folder/f'{frame:03}.jpg',quality=92,subsampling=0)
        write(d.OUT/'motion.csv',rows)
        encode(folder,d.OUT/'ridge-walk.mp4')
if __name__=='__main__':
    p=argparse.ArgumentParser()
    p.add_argument('mode',choices=('stills','motion'))
    args=p.parse_args()
    (stills if args.mode=='stills' else motion)()
