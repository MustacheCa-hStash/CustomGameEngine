"""Compare isolated twig parent samples from BakeSprayBough --twig-repair."""

import argparse
import csv
import math
import struct
import sys
from pathlib import Path

import numpy as np
from CompareOakMaskEdges import measure


def load_sample(path):
    data = path.read_bytes()
    vertices, indices = struct.unpack_from("<II", data)
    if len(data) != 8 + vertices * 40 + indices * 4 or indices % 3:
        raise ValueError(f"Invalid parent sample in {path}")
    points = np.frombuffer(data, "<f4", vertices * 10, 8).reshape(-1, 10)
    faces = np.frombuffer(data, "<u4", indices, 8 + vertices * 40).reshape(-1, 3)
    return points, faces


def raster(sample, focus, yaw, pitch, distance, light):
    """Two-sided, single pixel-center sampling with perspective-correct first hits."""
    vertices, triangles = sample
    yaw, pitch = np.radians([yaw, pitch])
    right = np.array([np.cos(yaw), 0, -np.sin(yaw)])
    back = np.array([np.sin(yaw)*np.cos(pitch), np.sin(pitch), np.cos(yaw)*np.cos(pitch)])
    view = (vertices[:, :3] - focus) @ np.stack([right, np.cross(back, right), back]).T
    depth = distance - view[:, 2]
    width, height = (2048, 2048) if light else (3840, 2160)
    # The light diagnostic uses a fixed 24 m span centered on the oak.
    scale = width / 24 if light else (height / (2 * math.tan(math.pi/8))) / depth
    xy = view[:, :2] * np.broadcast_to(scale, depth.shape)[:, None] * [1, -1] + [width/2, height/2]
    zbuffer = np.full(width*height, np.inf, np.float32)
    normals = np.zeros((width*height, 3), np.float32)
    for start in range(0, len(triangles), 2048):
        faces = triangles[start:start+2048]
        screen = xy[faces]
        lower = np.maximum(np.ceil(screen.min(1)-0.5).astype(int), 0)
        upper = np.minimum(np.floor(screen.max(1)-0.5).astype(int), [width-1, height-1])
        size = np.maximum(upper-lower+1, 0)
        counts = size[:, 0]*size[:, 1]
        face = np.repeat(np.arange(len(faces)), counts)
        offset = np.arange(len(face)) - np.repeat(np.cumsum(counts)-counts, counts)
        x = lower[face, 0]+offset%size[face, 0]
        y = lower[face, 1]+offset//size[face, 0]
        a, b, c = screen.transpose(1, 0, 2)
        u, v = b-a, c-a
        dx, dy = x+0.5-a[face, 0], y+0.5-a[face, 1]
        det = (u[:, 0]*v[:, 1]-u[:, 1]*v[:, 0])[face]
        with np.errstate(divide="ignore", invalid="ignore"):
            wb = (dx*v[face, 1]-dy*v[face, 0])/det
            wc = (u[face, 0]*dy-u[face, 1]*dx)/det
            keep = (wb >= 0) & (wc >= 0) & (wb+wc <= 1)
        face, wb, wc = faces[face[keep]], wb[keep], wc[keep]
        pixel = y[keep]*width+x[keep]
        weights = np.stack([1-wb-wc, wb, wc], axis=1)
        if not light:
            weights /= depth[face]
            weights /= weights.sum(1)[:, None]
        z = (weights * depth[face]).sum(1).astype(np.float32)
        np.minimum.at(zbuffer, pixel, z)
        nearest = z == zbuffer[pixel]
        normals[pixel[nearest]] = np.einsum("ij,ijk->ik", weights[nearest], vertices[face[nearest], 3:6])
    return zbuffer.reshape(height,width), normals


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("samples", type=Path)
    parser.add_argument("--focus", nargs=3, type=float, default=[-.0098806394,7.31548654,-.0650237372])
    args = parser.parse_args()
    writer = csv.writer(sys.stdout, lineterminator="\n")
    writer.writerow(["parent","view","cut","tris","iou","edge_beyond_1px","common_pixels","depth_p95_mm","normal_p95_deg"])
    for reference in sorted(args.samples.glob("*-reference.mesh")):
        parent = reference.name.split("-")[0]
        for view, yaw, pitch, distance in [("41m",132,-5.46,41), ("100m",132,-2.235,100), ("sun",102,49,41)]:
            def render(sample):
                return raster(sample, args.focus, yaw, pitch, distance, view == "sun")
            refz, refn = render(load_sample(reference))
            for cut in ["previous", "refit"]:
                sample = load_sample(args.samples / f"{parent}-{cut}.mesh")
                z, n = render(sample)
                mask, ref = np.isfinite(z), np.isfinite(refz)
                shared = ref & mask
                metrics = next(measure(ref, [(1, len(sample[1]), mask)]))
                a, b = n[shared.ravel()], refn[shared.ravel()]
                norm = np.linalg.norm(a,axis=1)*np.linalg.norm(b,axis=1)
                valid = norm > 1e-10
                angle = np.degrees(np.arccos(np.clip(np.sum(a[valid]*b[valid],axis=1)/norm[valid], -1, 1)))
                writer.writerow([parent, view, cut, len(sample[1]), metrics[2], metrics[-1], shared.sum(),
                                 np.percentile(abs(z[shared]-refz[shared])*1000,95), np.percentile(angle,95)])


if __name__ == "__main__":
    main()
