"""Compare the registered meadow captures and check that camera controls preserve every shadow cascade."""
import csv
from statistics import median

import numpy as np
from PIL import Image, ImageChops, ImageDraw, ImageFont

import camera_meadow as study


def rgb(path):
    with Image.open(path) as image:
        return image.convert("RGB")


def panels(path, frames, width):
    font = ImageFont.truetype("C:/Windows/Fonts/segoeui.ttf", 22)
    height = round(width * frames[0][1].height / frames[0][1].width)
    sheet = Image.new("RGB", (width * len(frames), height + 42), (28, 32, 28))
    draw = ImageDraw.Draw(sheet)
    for i, (label, frame) in enumerate(frames):
        draw.text((i*width+12, 5), label, font=font, fill="white")
        sheet.paste(frame.resize((width,height), Image.Resampling.LANCZOS), (i*width,42))
    sheet.save(path, quality=95, subsampling=0)


def visual():
    review = study.REVIEW
    for pose in study.far.CAMERAS:
        old = rgb(review/"original"/(pose+".png"))
        new = rgb(review/"forward/4k/all"/(pose+".png"))
        legacy = rgb(study.far.OUT/"review/both"/(pose+".png"))
        if ImageChops.difference(legacy, old).getbbox():
            raise ValueError(f"{pose} control diverges from the previous scene")
        panels(review/(pose+"-bank.jpg"), [("Original meadow",old), ("Fresh growth",new)], 960)
        if pose != "downhill":
            continue
        # Registered crop from the 4K render, including the right-hand oak as a landmark.
        region = (1792, 936, 3456, 1504)
        crops = [(label, frame.crop(region)) for label, frame in (("Original meadow", old), ("Fresh growth", new))]
        panels(review/"downhill-detail.jpg", crops, 832)
        blink = []
        for label, frame in crops:
            frame.thumbnail((1248, 426), Image.Resampling.LANCZOS)
            draw = ImageDraw.Draw(frame)
            draw.rectangle((0, 0, 180, 39), fill=(28,32,28))
            draw.text((10,4), label, fill="white", font=ImageFont.truetype("C:/Windows/Fonts/segoeui.ttf",22))
            blink.append(frame)
        blink[0].save(review/"downhill-detail.webp", save_all=True, append_images=blink[1:],
                      duration=1400, loop=0, lossless=True)
        delta = np.asarray(ImageChops.difference(new, old)).max(axis=2)
        shadow = np.asarray(rgb(study.far.OUT/"review/checks/candidate/downhill-shadow-visibility.png"))[:,:,0]
        no_oak = np.asarray(rgb(study.far.OUT/"review/checks/no-oak-shadow/downhill-shadow-visibility.png"))[:,:,0]
        changed = delta >= 6
        print("Downhill pixels changing by >=6 display levels:", changed.sum(),
              "sunlit:", (changed & (shadow>=230)).sum(),
              "oak shadow:", (changed & (shadow<=64) & (no_oak>=192)).sum())
        layers = [(label, rgb(review/"forward/4k"/layer/"downhill.png"))
                  for label, layer in (("Tall + short", "all"), ("Short only", "short"), ("Both hidden", "bare"))]
        panels(review/"camera-layers.jpg", layers, 640)


def timings():
    review = study.REVIEW
    results = []
    counts = ("camera_triangles", *(f"cascade{i}_triangles" for i in range(3)))
    for pose in study.far.CAMERAS:
        reference = None
        for size, dims in study.SIZES.items():
            for layer in study.LAYERS:
                runs = []
                for run in ("forward", "reverse"):
                    folder = review/run/size/layer
                    with Image.open(folder/(pose+".png")) as im:
                        assert im.size == dims
                    with (folder/(pose+".timings.csv")).open() as stream:
                        runs.append(list(csv.DictReader(stream)))
                assert all(len(r)==12 for r in runs)
                samples = runs[0] + runs[1]
                fixed = {}
                for key in counts:
                    values = {int(f[key]) for f in samples}
                    assert len(values) == 1, (pose, size, layer, key)
                    fixed[key] = values.pop()
                cascades = [fixed[key] for key in counts[1:]]
                if reference is None:
                    reference = cascades
                assert cascades == reference, (pose, size, layer, cascades, reference)
                times = {k: round(median(float(s[k]) for s in samples),3)
                         for k in ("gpu_camera_ms","gpu_frame_ms","gpu_shadow_ms")}
                medians = [median(float(s["gpu_camera_ms"]) for s in run) for run in runs]
                results.append(dict(pose=pose, size=size, layer=layer, **times, **fixed,
                                    forward_camera_ms=round(medians[0],3), reverse_camera_ms=round(medians[1],3)))
    with (review/"timings.csv").open("w", newline="") as output:
        writer = csv.DictWriter(output, fieldnames=results[0])
        writer.writeheader()
        writer.writerows(results)
    counters = []
    for layer in study.LAYERS:
        with (review/"counters"/layer/"downhill.timings.csv").open() as stream:
            samples = list(csv.DictReader(stream))
        assert len(samples) == 12
        counts = {k: int(median(int(s[k]) for s in samples))
                  for k in ("camera_triangles","camera_mesh_vs_invocations","camera_mesh_ps_invocations")}
        assert min(counts.values()) > 0
        counters.append(dict(layer=layer, **counts))
    with (review/"downhill-counters.csv").open("w", newline="") as output:
        writer = csv.DictWriter(output, fieldnames=counters[0])
        writer.writeheader()
        writer.writerows(counters)


if __name__ == "__main__":
    visual()
    timings()
