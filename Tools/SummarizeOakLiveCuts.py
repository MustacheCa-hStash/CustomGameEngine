"""Summarize the live oak experiment without launching either renderer."""
import csv
from collections import defaultdict
from statistics import median

import numpy as np
from PIL import Image, ImageDraw, ImageFont

from BenchmarkOakLiveCuts import OUT, native_paths
from CompareOakMaskEdges import read_mask
from DecodeOakBoughVoxels import write_csv


def read(path):
    with path.open(newline="") as stream:
        return list(csv.DictReader(stream))


def review_sheet():
    font = lambda size: ImageFont.truetype("segoeui.ttf", size)
    sheet = Image.new("RGB", (1110, 790), "#fafaf7")
    pen = ImageDraw.Draw(sheet)
    pen.text((25,15),"White oak at 41 m and 100 m",font=font(30),fill="#152630")
    pen.text((25,62),"Each row uses the same 4K camera. Detail crops show individual source pixels at 2×.",font=font(18),fill="#40545b")
    arm="canopy1" if (OUT/"canopy-timing.csv").exists() else "tiered"
    timings=read(OUT/("canopy-timing.csv" if arm=="canopy1" else "guard-timing.csv"))
    quality=read(OUT/("canopy-quality.csv" if arm=="canopy1" else "guard.csv"))
    for index,d in enumerate((41,100)):
        top=106+index*314
        native,fine=native_paths("crown",d,132)
        ref=read_mask(fine,True)
        ys,xs=np.where(ref)
        bbox=(xs.min()-8,ys.min()-8,xs.max()+9,ys.max()+9)
        row=next(r for r in timings if r.get("subject","crown")=="crown" and int(r["d"])==d and r["arm"]==arm)
        with Image.open(OUT/row["capture"]) as image:silhouette=image.crop(bbox).convert("RGB")
        silhouette.thumbnail((333,247),Image.Resampling.LANCZOS)
        pen.text((24,top),f"{d} m   Reusable canopy",font=font(21),fill="#152630")
        maskrow=next(r for r in quality if r["subject"]=="crown" and int(r["d"])==d and r["yaw"]=="132" and r["arm"]==arm)
        # Pick an upper crown edge on the reference, independent of candidate errors.
        y=round(ys.min()+.27*(ys.max()-ys.min()))
        left=int(np.percentile(np.where(ref[y-4:y+5])[1],97))-80
        upper=y-55
        sx,sy=silhouette.width/(bbox[2]-bbox[0]),silhouette.height/(bbox[3]-bbox[1])
        ImageDraw.Draw(silhouette).rectangle(((left-bbox[0])*sx,(upper-bbox[1])*sy,
            (left+160-bbox[0])*sx,(upper+110-bbox[1])*sy),outline="#fcefb7",width=2)
        sheet.paste(silhouette,(24,top+38))
        subset=np.s_[upper:upper+110,left:left+160]
        for i,(label,mask) in enumerate((("Native Nanite",read_mask(native,True)),("Reusable canopy",read_mask(OUT/maskrow["capture"])))):
            r,m=ref[subset],mask[subset]
            pixels=np.full((*r.shape,3),[241,242,235],np.uint8)
            pixels[m&r]=[40,61,52];pixels[r&~m]=[198,75,38];pixels[m&~r]=[36,126,184]
            x=383+i*357
            pen.text((x,top),label,font=font(21),fill="#152630")
            sheet.paste(Image.fromarray(pixels).resize((320,220),Image.Resampling.NEAREST),(x,top+43))
        pen.line((24,top+292,1080,top+292),fill="#ccd2cd")
    for x,color,label in ((24,"#283d34","Shared foreground"),(300,"#c64b26","Missing from cut"),(565,"#247eb8","Added by cut")):
        pen.rectangle((x,750,x+17,767),fill=color)
        pen.text((x+26,744),label,font=font(18),fill="#263431")
    sheet.save(OUT/"crown-comparison.png")


def summarize():
    grouped = defaultdict(list)
    for dataset in ("timing", "hybrid-timing", "guard-timing", "canopy-timing", "order-timing", "fenced-timing"):
        for row in read(OUT / f"{dataset}.csv"):
            grouped[dataset, row.get("subject","crown"), int(row["d"]), row["arm"], float(row.get("limit",1))].append(row)
    results = []
    measures = ("tris", "draws", "camera_leaf_triangles", "camera_spray_wood_triangles", "camera_tree_trunk_triangles",
                "camera_tree_draws", "gpu_camera_ms", "gpu_shadow_ms", "gpu_frame_ms", "camera_selection_cpu_ms",
                "camera_tree_submit_ms", "cpu_ms", "shadow_tris")
    for (dataset, subject, distance, arm, limit), samples in grouped.items():
        assert len(samples) == 4
        row = dict(series=dataset, subject=subject, distance=distance, arm=arm, limit=limit)
        for measure in measures:
            row[measure] = median(float(s[measure]) for s in samples)
        for measure in ("gpu_camera_ms", "gpu_frame_ms", "cpu_ms"):
            values = [float(s[measure]) for s in samples]
            row[measure + "_min"], row[measure + "_max"] = min(values), max(values)
        walls=[float(s["fenced_frame_ms"]) for s in samples if s.get("fenced_frame_ms")]
        row["fenced_frame_ms"]=median(walls) if walls else ""
        row["fenced_frame_ms_min"]=min(walls) if walls else ""
        row["fenced_frame_ms_max"]=max(walls) if walls else ""
        results.append(row)
    write_csv(OUT / "performance-summary.csv", results)

    quality = defaultdict(dict)
    for dataset in ("verification", "crown-screen", "hybrid-quality", "guard", "canopy-quality"):
        for row in read(OUT / f"{dataset}.csv"):
            subject, d, arm, limit = row["subject"], int(row["d"]), row["arm"], float(row["limit"])
            quality[subject, d, arm, limit][int(row["yaw"])] = row
    scores = []
    for (subject, distance, arm, limit), views in quality.items():
        failed = sorted(y for y, row in views.items() if row["common_pass"] != "True" or row["own_pass"] != "True")
        scores.append(dict(subject=subject, distance=distance, arm=arm, limit=limit, views=len(views),
                           passed=len(views) - len(failed), failed_yaws=" ".join(map(str, failed)),
                           min_iou_margin=min(float(r["iou"]) - float(r["gate_iou"]) for r in views.values()),
                           min_edge_margin=min(float(r["gate_edge"]) - float(r["edge"]) for r in views.values())))
    write_csv(OUT / "quality-summary.csv", scores)

    moving = defaultdict(list)
    for row in read(OUT / "motion.csv"):
        moving[row["subject"], row["wind"], row["arm"]].append(row)
    stats = []
    for (subject, wind, arm), frames in moving.items():
        assert len(frames) == 62
        transitions = [float(r["temporal_fraction"]) for r in frames if int(r["frame"])]
        stats.append(dict(subject=subject, wind=wind, arm=arm, frames=len(frames),
                          worst_iou=min(float(r["iou"]) for r in frames),
                          worst_edge=max(float(r["edge"]) for r in frames),
                          temporal_p95=float(np.percentile(transitions, 95))))
    write_csv(OUT / "motion-summary.csv", stats)
    review_sheet()


if __name__ == "__main__":
    summarize()
