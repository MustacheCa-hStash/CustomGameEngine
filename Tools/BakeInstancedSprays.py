"""Bake each repeated modeled foliage primitive with the existing fixed-LOD baker."""
import argparse
import concurrent.futures
import hashlib
import json
from pathlib import Path
import struct
import subprocess
import tempfile


LEAF = "--rim-anchors uv-v --lod1 30 --lod2 10 --lod3 3 --lod4 1 --lod4-component-triangles 33 --lod4-refinement-error 0.006066 --leaf-proxies uv-v --transition-min-lod 3"
WOOD = "--lod1 50 --lod2 20 --lod3 8 --lod4 3 --weld-normal-splits on --shared-coarse-triangles 425 --shared-far-triangles 950 --shared-far-uv-weight 2 --transition-min-lod 0"


def primitives(asset):
    with asset.open("rb") as stream:
        magic, version, _, size, kind = struct.unpack("<4s4I", stream.read(20))
        if (magic, version, kind) != (b"glTF", 2, 0x4e4f534a):
            raise ValueError("Expected a binary glTF 2.0 asset.")
        doc = json.loads(stream.read(size))
    result = []
    mesh_index = 0
    static_parts = []
    # Use GltfLoader's active-scene depth-first stack order for the scaffold index.
    pending = list(doc["scenes"][doc.get("scene", 0)]["nodes"])
    while pending:
        node = doc["nodes"][pending.pop()]
        pending.extend(node.get("children", []))
        if "mesh" not in node:
            continue
        parts = doc["meshes"][node["mesh"]]["primitives"]
        inst = node.get("extensions", {}).get("EXT_mesh_gpu_instancing")
        if not inst:
            static_parts.extend(range(mesh_index, mesh_index + len(parts)))
            mesh_index += len(parts)
            continue
        pair = {}
        for i, part in enumerate(parts):
            material = doc["materials"][part["material"]]
            if material.get("alphaMode", "OPAQUE") != "OPAQUE":
                raise ValueError("Cutout or blended foliage needs an alpha-aware recipe.")
            leaf = "KHR_materials_diffuse_transmission" in material.get("extensions", {})
            name = node.get("name", "Mesh")
            if len(parts) > 1:
                name += f" / Primitive {i + 1}"
            if leaf in pair:
                raise ValueError("Each modeled shoot must have one leaf and one wood primitive.")
            pair[leaf] = name
        if len(pair) != 2:
            raise ValueError("Expected a modeled leaf and wood pair.")
        result.append((pair[True], pair[False]))
        mesh_index += len(parts)
    names = [n for pair in result for n in pair]
    if not result or len(set(names)) != len(names) or len(static_parts) != 1:
        raise ValueError("Expected uniquely named shoots and one static scaffold.")
    return result, static_parts[0]


def bake(asset, baker, jobs=3):
    pairs, trunk = primitives(asset)
    parts = [(name, recipe) for pair in pairs for name, recipe in zip(pair, (LEAF, WOOD))]
    # Qualify every member of a publication with the same input and tool versions.
    # Readers keep using the previous complete set until the manifest is replaced.
    stamp = hashlib.sha256()
    for path in (asset, baker, baker.with_name("BakeSprayBough.exe"), Path(__file__)):
        with path.open("rb") as f:
            stamp.update(hashlib.file_digest(f, "sha256").digest())
    with tempfile.TemporaryDirectory(dir=asset.parent) as tmp:
        stage = Path(tmp)
        def run(entry):
            name, recipe = entry
            key = stamp.copy()
            key.update(name.encode())
            target = stage / f"spray-{key.hexdigest()[:24]}.fixedlod"
            command = [baker, asset, name, target, "--samples", "4", "--uv-weight", "0.1", "--spray-groups", "8"]
            result = subprocess.run([*command, *recipe.split()], capture_output=True, text=True)
            if result.returncode and recipe == WOOD and "shared far level collapsed a UV triangle" in result.stderr:
                print(f"{name}: keeping the component-wise far level after UV rejection", flush=True)
                safe = recipe.replace("--shared-far-triangles 950 --shared-far-uv-weight 2 ", "")
                subprocess.run([*command, *safe.split()], check=True, stdout=subprocess.DEVNULL)
            elif result.returncode:
                raise RuntimeError(f"{name}: {result.stderr}")
            print(f"Baked {name}", flush=True)
            return target
        with concurrent.futures.ThreadPoolExecutor(max_workers=jobs) as pool:
            files = list(pool.map(run, parts))
        lines = []
        for leaf, wood in zip(files[::2], files[1::2]):
            hierarchy = leaf.with_suffix(".treelod")
            subprocess.run([baker.with_name("BakeSprayBough.exe"), "--hierarchy", asset, leaf, wood,
                            str(trunk), hierarchy, "96", "384"], check=True, stdout=subprocess.DEVNULL)
            print(f"Baked hierarchy for {leaf.stem}", flush=True)
            lines.append(f"{leaf.name} {wood.name} {hierarchy.name}\n")
        manifest = stage / asset.with_suffix(".spraylods").name
        manifest.write_text("".join(lines), encoding="utf-8")
        for path in files:
            for source in (path, Path(str(path)+".txt")):
                source.replace(asset.parent / source.name)
        for hierarchy in stage.glob("*.treelod"):
            hierarchy.replace(asset.parent / hierarchy.name)
        manifest.replace(asset.parent / manifest.name)


if __name__ == "__main__":
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("asset", type=Path)
    ap.add_argument("baker", type=Path)
    args = ap.parse_args()
    bake(args.asset.resolve(), args.baker.resolve())
