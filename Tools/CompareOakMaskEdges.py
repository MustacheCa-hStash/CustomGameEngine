"""Measure one-pixel outline agreement in the saved matched 4K oak captures."""

import argparse
import csv
from pathlib import Path
import sys

import numpy as np
from PIL import Image


def read_mask(path, unreal=False):
    with Image.open(path) as image:
        if image.size != (3840, 2160):
            raise ValueError(f"Expected a 4K capture in {path}")
        if unreal:
            red, green, blue = np.asarray(image.convert("RGB")).transpose(2, 0, 1)
            return (green > red) & (green > blue)
        return np.asarray(image.convert("L")) != 0


def outline(mask):
    # A foreground pixel borders a gap if an in-frame axial neighbor is empty.
    edge = np.zeros_like(mask)
    edge[1:] |= mask[1:] & ~mask[:-1]
    edge[:-1] |= mask[:-1] & ~mask[1:]
    edge[:, 1:] |= mask[:, 1:] & ~mask[:, :-1]
    edge[:, :-1] |= mask[:, :-1] & ~mask[:, 1:]
    return edge


def within_one_pixel(edge):
    expanded = edge.copy()
    expanded[1:] |= edge[:-1]
    expanded[:-1] |= edge[1:]
    expanded[:, 1:] |= edge[:, :-1]
    expanded[:, :-1] |= edge[:, 1:]
    return expanded


def first_row(path):
    with path.open(newline="") as stream:
        return next(csv.DictReader(stream))


def measure(reference, candidates):
    ref_edge = outline(reference)
    ref_band = within_one_pixel(ref_edge)
    for budget, triangles, mask in candidates:
        edge = outline(mask)
        missing = np.count_nonzero(ref_edge & ~within_one_pixel(edge))
        extra = np.count_nonzero(edge & ~ref_band)
        total = np.count_nonzero(ref_edge) + np.count_nonzero(edge)
        yield (budget, triangles, np.count_nonzero(reference & mask) / np.count_nonzero(reference | mask),
               missing, extra, (missing + extra) / total)


def custom_cuts(root, distance):
    series = root / "oak-gpu-selection" / "matched"
    for path in sorted((series / "coverage").glob(f"q{distance}-*.png")):
        row = first_row(path.with_suffix(".timings.csv"))
        yield float(path.stem.split("-")[1]), int(row["camera_triangles"]), read_mask(path)
    row = first_row(series / "repeat" / f"q{distance}-direct.timings.csv")
    # The shaded timing pass also draws the sky and ground. Use the tree counters.
    triangles = sum(int(row[key]) for key in
                    ("camera_leaf_triangles", "camera_spray_wood_triangles", "camera_tree_trunk_triangles"))
    yield 1.0, triangles, read_mask(series / "mask" / f"q{distance}-direct.png")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build", type=Path, default=Path(__file__).resolve().parents[1] / "build")
    root = parser.parse_args().build
    matched = root / "oak-triangles-matched"
    unreal = root / "unreal-nanite-oak" / "unreal"
    with (matched / "unreal-counts.csv").open(newline="") as stream:
        counts = {int(r["distance"]): int(r["triangles"]) for r in csv.DictReader(stream)
                  if r["condition"] == "frustum" and int(r["bricks"]) == 0}
    writer = csv.writer(sys.stdout, lineterminator="\n")
    writer.writerow(("distance_m", "renderer", "budget_px", "triangles", "iou",
                     "unmatched_reference_edges", "unmatched_cut_edges", "edge_fraction_beyond_1px"))
    for distance in (14, 18, 24, 41, 100):
        reference = read_mask(unreal / f"matched_fixed_q{distance}_Mask.png", unreal=True)
        cut = read_mask(unreal / f"matched_frustum_q{distance}_Mask.png", unreal=True)
        for row in measure(reference, [(1.0, counts[distance], cut)]):
            writer.writerow((distance, "Nanite", *row))
        reference = read_mask(matched / f"fixed_q{distance}.png")
        for row in measure(reference, custom_cuts(root, distance)):
            writer.writerow((distance, "Custom", *row))


if __name__ == "__main__":
    main()
