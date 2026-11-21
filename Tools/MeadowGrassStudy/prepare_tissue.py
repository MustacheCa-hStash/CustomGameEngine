"""Derive the five lamina albedos from one ImageGen tissue plate."""
from pathlib import Path

import numpy as np
from PIL import Image, ImageFilter

folder = Path(__file__).resolve().parents[2] / "assets/source/meadow-grass-study"
source = Image.open(folder / "blade-tissue.png").convert("RGB")
for label, center, span, rgb, drying in (
    ("green", 0.51, 0.42, (83,113,52), 0.015),
    ("cool", 0.51, 0.29, (73,105,62), 0.035),
    ("olive", 0.39, 0.49, (106,125,59), 0.08),
    ("fading", 0.71, 0.53, (122,131,65), 0.42),
    ("straw", 0.43, 0.54, (157,136,84), 0.86),
):
    left, right = (int(source.width * (center + d*span)) for d in (-0.5,0.5))
    strip = np.array(source.crop((left,0,right,source.height)).resize((256,1024),Image.Resampling.LANCZOS),
                     dtype=float)
    lum = strip @ np.array((0.27,0.65,0.08))
    contrast = (lum / np.median(lum)) ** 0.94
    row = np.linspace(1,0,strip.shape[0])[:,None]
    vein = (lum - np.mean(lum,axis=1,keepdims=True)) / 120
    tip = np.clip((row-(1-drying))/max(0.02,drying) + vein*0.28,0,1)
    target = np.array(rgb) + (np.array((185,154,90))-rgb)*tip[:,:,None]
    color = target*contrast[:,:,None] + (strip - strip.mean(axis=(0,1)))*0.12
    Image.fromarray(np.uint8(np.clip(color,0,255))).save(folder / f"blade-{label}.png")
    if label == "green":
        # Shallow longitudinal ridges. Preserve the source's vein spacing, with
        # the low-frequency relief from the leaf cross section handled by geometry.
        fine = np.asarray(Image.fromarray(lum.astype("uint8")).filter(ImageFilter.GaussianBlur(1.15)),float)
        dx = np.gradient(fine,axis=1)*0.026
        dy = np.gradient(fine,axis=0)*0.003
        normals = np.stack((-dx,dy,np.ones_like(lum)),axis=-1)
        normals /= np.linalg.norm(normals,axis=-1,keepdims=True)
        Image.fromarray(np.uint8((normals*0.5+0.5)*255)).save(folder/"blade-ribs.png")
