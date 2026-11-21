"""Build the isolated meadow lighting viewer and run a registered benchmark camera."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

import oak_baseline as base


HERE = Path(__file__).resolve().parent
OUT = base.ROOT/"build/meadow-oak-refinement"
PATCH = HERE/"meadow_lighting.patch"


def prepare_viewer(*, upstream=None, out=OUT, patch=PATCH, destination=None):
    upstream = upstream or base.ROOT/"build/oak-crown-optimized/source"
    private = out/"source"
    if not private.exists():
        shutil.copytree(upstream,private)
    files = [line[6:] for line in patch.read_text().splitlines() if line.startswith("+++ b/")]
    with tempfile.TemporaryDirectory(dir=out) as tmp:
        work = Path(tmp)
        for name in files:
            (work/name).parent.mkdir(parents=True,exist_ok=True)
            shutil.copyfile(upstream/name,work/name)
        subprocess.run(["git","apply","--no-index","--ignore-space-change",patch],cwd=work,
                       env=dict(os.environ,GIT_CEILING_DIRECTORIES=str(work.parent)),check=True)
        for name in files:
            if (private/name).read_bytes().splitlines() != (work/name).read_bytes().splitlines():
                shutil.copyfile(work/name,private/name)
    cache = (base.ROOT/"build/CMakeCache.txt").read_text().splitlines()
    cmake = next(s.split("=",1)[1] for s in cache if s.startswith("CMAKE_COMMAND:INTERNAL="))
    compiler = out/"compiler"
    if not (compiler/"CMakeCache.txt").exists():
        subprocess.run([cmake,"-S",private,"-B",compiler,"-G","Visual Studio 17 2022","-A","x64"],check=True)
    subprocess.run([cmake,"--build",compiler,"--config","Release","--target","CustomGameEngine",
                    "--","/m:1","/nr:false"],check=True)
    binary = compiler/"Release/CustomGameEngine.exe"
    viewer = destination or base.STAGE/"MeadowOakLighting.exe"
    if not viewer.exists() or viewer.read_bytes()!=binary.read_bytes():
        shutil.copy2(binary,viewer)
    return viewer


def ground_visibility():
    # Read actual GLB instance streams, including the source's float32 rounding.
    model = base.oak.OakGlb(base.MODEL)
    node = next(n for n in model.doc["nodes"] if n["name"]=="Full grass strip")
    attrs = node["extensions"]["EXT_mesh_gpu_instancing"]["attributes"]
    transforms = {key:model.array(a).tolist() for key,a in attrs.items()}
    layout = json.dumps(transforms,sort_keys=True).encode()
    script = HERE/"bake_ground_visibility.py"
    signature = hashlib.sha256(script.read_bytes()+base.TUFT.read_bytes()+layout).hexdigest()
    destination = base.ASSETS/"MeadowGroundVisibility.png"
    stamp = OUT/"ground-visibility.sha256"
    if destination.exists() and stamp.exists() and stamp.read_text()==signature:
        return
    blender = shutil.which("blender") or Path(os.environ["ProgramFiles"])/"Blender Foundation/Blender 5.0/blender.exe"
    with tempfile.TemporaryDirectory(dir=OUT) as tmp:
        placements = Path(tmp)/"placements.json"
        placements.write_bytes(layout)
        output = Path(tmp)/destination.name
        subprocess.run([blender,"-b","--python-exit-code","1","-P",script,"--",placements,output],check=True)
        output.replace(destination)
    stamp.write_text(signature)


if __name__=="__main__":
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("mode",choices=("capture","view"))
    ap.add_argument("--pose",choices=base.VIEWS,default="edge")
    ap.add_argument("--pass",dest="arm",choices=("after","shadows","material"),default="after")
    ap.add_argument("--open",action="store_true")
    ap.add_argument("--lighting",choices=("combined","shadow-visibility"),default="combined")
    args = ap.parse_args()
    meta = base.stage()
    OUT.mkdir(parents=True,exist_ok=True)
    viewer = prepare_viewer()
    ground_visibility()
    env = os.environ.copy()
    for name in ("MEADOW_SHADOW_CONTROL","MEADOW_MATERIAL_CONTROL"):
        env.pop(name,None)
    if args.arm=="shadows":
        env["MEADOW_MATERIAL_CONTROL"]="1"
    if args.arm=="material":
        env["MEADOW_SHADOW_CONTROL"]="1"
    base.run(args.pose,meta["controlFocus" if args.open else "focus"],args.mode,args.lighting,args.open,
             viewer=viewer,output_dir=OUT/args.arm,environment=env)
