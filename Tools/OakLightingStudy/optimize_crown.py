"""Build and view the open-crown oak with source-baked GPU triangle LODs."""
import argparse
import copy
import filecmp
import hashlib
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

import numpy as np

import upper_crown as crown
from upper_crown import oak, sprays

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent))
import BakeInstancedSprays as baker

OUT = oak.lighting.ROOT / "build/oak-crown-optimized"
SCENE = OUT / "scene"
ASSET = SCENE / "assets" / (oak.NAME + ".glb")
PORTABLE = crown.MODEL.with_name("WhiteOak-OpenCrown-Simplified.glb")
GPU_FLAGS = ["--lod", "auto", "--spray-mode", "hierarchy", "--tree-submit", "batched",
             "--spray-culling", "on", "--spray-local-lod", "on"]
REFERENCE_GUARD = "--guard-spray-reference"


def copy_changed(source, dest):
    if not dest.exists() or not filecmp.cmp(source, dest, shallow=False):
        shutil.copy2(source, dest)


def viewer():
    baseline = sprays.OUT / "transmission-source"
    if not baseline.exists():
        sprays.prepare_transmission(sprays.prepare())
    patch = HERE / "spray_families.patch"
    src = OUT / "source"
    stamp = OUT / "viewer-patch.sha256"
    digest = hashlib.sha256(patch.read_bytes()).hexdigest()
    source_updated = False
    if not src.exists():
        with tempfile.TemporaryDirectory(dir=OUT) as tmp:
            stage = Path(tmp) / "source"
            shutil.copytree(baseline, stage)
            git_env = dict(os.environ, GIT_CEILING_DIRECTORIES=str(stage.parent))
            subprocess.run(["git", "apply", "--no-index", "--ignore-space-change", patch],
                           cwd=stage, env=git_env, check=True)
            stage.rename(src)
        source_updated = True
        stamp.write_text(digest)
    elif not stamp.exists() or stamp.read_text() != digest:
        # Reapply a revised overlay to pristine files; leave the frozen viewer untouched.
        files = [line[6:] for line in patch.read_text().splitlines() if line.startswith("+++ b/")]
        with tempfile.TemporaryDirectory(dir=OUT) as tmp:
            stage = Path(tmp) / "source"
            for file in files:
                (stage / file).parent.mkdir(parents=True, exist_ok=True)
                shutil.copy2(baseline / file, stage / file)
            subprocess.run(["git", "apply", "--no-index", "--ignore-space-change", patch], cwd=stage,
                           env=dict(os.environ, GIT_CEILING_DIRECTORIES=tmp), check=True)
            for file in files:
                if (stage / file).read_text() != (src / file).read_text():
                    shutil.copyfile(stage / file, src / file)
                    source_updated = True
        stamp.write_text(digest)
    compiler = OUT / "compiler"
    binary = compiler / "Release/CustomGameEngine.exe"
    if source_updated or not binary.exists() or patch.stat().st_mtime_ns > binary.stat().st_mtime_ns:
        cache = (oak.lighting.ROOT / "build/CMakeCache.txt").read_text().splitlines()
        cmake = next(line.split("=", 1)[1] for line in cache if line.startswith("CMAKE_COMMAND:INTERNAL="))
        subprocess.run([cmake, "-S", src, "-B", compiler, "-G", "Visual Studio 17 2022", "-A", "x64"], check=True)
        subprocess.run([cmake, "--build", compiler, "--config", "Release", "--target", "CustomGameEngine",
                        "--parallel", "6"], check=True)
    copy_changed(binary, SCENE / sprays.SPRAY_EXE)


def portable():
    # Keep the source vertex streams (including wind UVs), compacting them with
    # each primitive's own baked indices. Embedded textures are copied verbatim.
    full = oak.OakGlb(ASSET)
    doc = copy.deepcopy(full.doc)
    doc["accessors"], doc["bufferViews"] = [], []
    payload = bytearray()
    families, _ = baker.primitives(ASSET)
    files = [row.split() for row in ASSET.with_suffix(".spraylods").read_text().splitlines()]
    cuts = {name: ASSET.parent / filename
            for pair, row in zip(families, files, strict=True)
            for name, filename in zip(pair, row[:2], strict=True)}

    def view(data, target=None):
        payload.extend(bytes(-len(payload) % 4))
        v = dict(buffer=0, byteOffset=len(payload), byteLength=len(data))
        if target:
            v["target"] = target
        doc["bufferViews"].append(v)
        payload.extend(data)
        return len(doc["bufferViews"]) - 1

    def store(index, values):
        a = dict(full.doc["accessors"][index])
        old = full.doc["bufferViews"][a["bufferView"]]
        values = np.ascontiguousarray(values, dtype=full.array(index).dtype)
        a.update(bufferView=view(values.tobytes(), old.get("target")), count=len(values), byteOffset=0)
        if "min" in a:
            a.update(min=values.min(axis=0).tolist(), max=values.max(axis=0).tolist())
        doc["accessors"].append(a)
        return len(doc["accessors"]) - 1

    for im in doc["images"]:
        old = full.doc["bufferViews"][im["bufferView"]]
        start = old.get("byteOffset", 0)
        im["bufferView"] = view(full.payload[start:start + old["byteLength"]])
    for node in doc["nodes"]:
        inst = node.get("extensions", {}).get("EXT_mesh_gpu_instancing", {}).get("attributes", {})
        for key, index in inst.items():
            inst[key] = store(index, full.array(index))
        parts = doc["meshes"][node["mesh"]]["primitives"]
        for i, part in enumerate(parts):
            index = part["indices"]
            raw = full.array(index)
            if inst:
                name = node["name"] + (f" / Primitive {i + 1}" if len(parts) > 1 else "")
                raw = oak.fixed_indices(cuts[name], level=4)
            used, remap = np.unique(raw, return_inverse=True)
            for key, original in part["attributes"].items():
                part["attributes"][key] = store(original, full.array(original)[used])
            part["indices"] = store(index, remap.reshape(-1, 1))
    payload.extend(bytes(-len(payload) % 4))
    doc["buffers"] = [dict(byteLength=len(payload))]
    doc["scenes"][0]["name"] = "Open crown oak with baked spray LOD 4"
    full.doc, full.payload = doc, payload
    full.write(PORTABLE)


def prepare(rebake=False):
    ASSET.parent.mkdir(parents=True, exist_ok=True)
    source_scene = crown.OUT / "scene"
    if not crown.MODEL.exists() or not (source_scene / "assets/MeadowDaySky.hdr").exists():
        source_scene = crown.stage()
    copy_changed(crown.MODEL, ASSET)
    for sky in (source_scene / "assets").glob("MeadowDaySky.*"):
        copy_changed(sky, ASSET.parent / sky.name)
    fixed = oak.lighting.ROOT / "build/Release/BakeFixedLod.exe"
    if not fixed.is_file() or not fixed.with_name("BakeSprayBough.exe").is_file():
        build = oak.lighting.ROOT / "build"
        cache = (build / "CMakeCache.txt").read_text().splitlines()
        cmake = next(line.split("=", 1)[1] for line in cache if line.startswith("CMAKE_COMMAND:INTERNAL="))
        subprocess.run([cmake, "--build", build, "--config", "Release", "--target", "BakeFixedLod",
                        "BakeSprayBough", "--parallel", "6"], check=True)
    manifest = ASSET.with_suffix(".spraylods")
    inputs = (ASSET, Path(baker.__file__), fixed, fixed.with_name("BakeSprayBough.exe"))
    complete = manifest.exists() and all((ASSET.parent / name).is_file()
                                          for name in manifest.read_text().split())
    if rebake or not complete or any(p.stat().st_mtime_ns > manifest.stat().st_mtime_ns for p in inputs):
        baker.bake(ASSET, fixed)
    if not PORTABLE.exists() or PORTABLE.stat().st_mtime_ns < manifest.stat().st_mtime_ns:
        portable()
    viewer()


def render(pose, trees, distance, yaw, budget, capture, unrestricted=False):
    flags = [*GPU_FLAGS, "--spray-pixel-error", str(budget)]
    if not unrestricted:
        flags.append(REFERENCE_GUARD)
    if distance is not None:
        flags.extend(["--distance", str(distance)])
        if pose == "grove" and distance >= 100:
            flags.extend(["--pitch-degrees","-2.235"])
    if yaw is not None:
        flags.extend(["--yaw-degrees", str(yaw)])
    sprays.render(SCENE, pose, capture, trees, extra=flags, exe=SCENE / sprays.SPRAY_EXE)


if __name__ == "__main__":
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--pose", choices=sprays.VIEWS, default="grove")
    ap.add_argument("--trees", type=int, choices=(1, 4), default=4)
    ap.add_argument("--distance", type=float)
    ap.add_argument("--yaw", type=float)
    ap.add_argument("--pixel-error", type=float, default=1)
    ap.add_argument("--capture", type=Path)
    ap.add_argument("--rebake", action="store_true")
    ap.add_argument("--unrestricted", action="store_true", help="Allow coarser camera parents and local spray cuts")
    ap.add_argument("--view", action="store_true")
    args = ap.parse_args()
    prepare(args.rebake)
    if args.view or args.capture:
        render(args.pose, args.trees, args.distance, args.yaw, args.pixel_error, args.capture, args.unrestricted)
