"""Capture the selected Nanite cluster buffers from the offscreen oak benchmark."""
import json
import os
from pathlib import Path
import runpy

import unreal

stage = Path(unreal.Paths.convert_relative_path_to_full(unreal.Paths.project_dir()))
destination = Path(os.environ.get("OAK_CUT_OUTPUT", stage.parent / "oak-assembly-step"))
destination.mkdir(parents=True, exist_ok=True)
jobs = []
joint = os.environ.get("OAK_CUT_SET") == "joint"
for distance, pitch in ((41, -5.46), (100, -2.235)):
    cases = [(132,1,"")] if os.environ.get("OAK_CUT_SET") != "validation" else [
        (132,.001,"-fine"), (90,1,"-yaw90"), (180,1,"-yaw180"),
        (90,.001,"-yaw90-fine"), (180,.001,"-yaw180-fine")]
    if joint:
        cases=[(yaw,b,s) for yaw,ys in ((132,""),(90,"-yaw90"),(180,"-yaw180"))
               for b,s in ((1,ys),(.001,ys+"-fine"))]
    for yaw,budget,suffix in cases:
        jobs.append(dict(name=f"{'joint' if joint else 'wood'}-cut-{distance}{suffix}", mode="Mask", distance=distance, pitch=pitch,
                         yaw=yaw, resolution=[3840, 2160], vertical_fov=45, isolated=True, capture_only=True,
                         frustum=1, hzb=0, hold_frames=100, max_pixels_per_edge=budget,
                         asset="Joint" if joint else "BranchStepWood", mesh_path="/Game/NaniteOakBench/"+("AssemblyStep07Joint" if joint else "BranchStep07WoodRoot")))
if os.environ.get("OAK_REUSABLE_BOUGH"):
    b=int(os.environ["OAK_REUSABLE_BOUGH"])
    extra_yaws=[int(y) for y in os.environ.get("OAK_EXTRA_YAWS","45,225").split(",") if y]
    for j in jobs:
        j["name"]=j["name"].replace("joint-cut",f"joint{b}-cut")
        j["mesh_path"]=f"/Game/NaniteOakBench/AssemblyStep{b:02d}Joint"
    for d,p in ((41,-5.46),(100,-2.235)):
        base=next(j for j in jobs if j["distance"]==d)
        for yaw in extra_yaws:
            for ratio,suffix in ((1,""),(.001,"-fine")):
                jobs.append(dict(base,name=f"joint{b}-cut-{d}-yaw{yaw}{suffix}",yaw=yaw,max_pixels_per_edge=ratio))
        for trial in (1,2):
            jobs.append(dict(base,name=f"reusable-b{b}-{d}-r{trial}",mode="Profile",hold_frames=65))
job_path = destination / ("capture-"+os.environ.get("OAK_CUT_SET","jobs")+os.environ.get("OAK_REUSABLE_BOUGH","")+".json")
job_path.write_text(json.dumps(jobs, indent=2))
os.environ["OAK_CAPTURE_JOBS"] = str(job_path)
rig = runpy.run_path(str(stage / "capture_unreal.py"))
original = rig["Rig"].step
command = rig["command"]
for setting in ("r.DumpGPU.Texture 0", "r.DumpGPU.Buffer 2", "r.DumpGPU.Explore 0",
                "r.DumpGPU.Upload 0", "r.DumpGPU.ConsoleVariables 0", "r.DumpGPU.PassParameters 1",
                "r.ShadowQuality 0"):
    command(setting)

def step(self):
    dump = self.entry and self.entry["mode"]=="Mask" and (not joint or (self.entry["yaw"]==132 and self.entry["max_pixels_per_edge"]==1))
    if dump and self.frame == 55:
        folder = destination / self.entry["name"].replace("wood-cut-","dump").replace("joint-cut-","joint-dump")
        filters = "|".join("*SW Rasterize (Triangles)*@" + buffer for buffer in
                           ("*VisibleClustersSWHW", "*ClusterPageData", "*AssemblyTransforms", "*Raster*Args*"))
        filters += "|*CalculateSafeRasterizerArgs*@*Raster*Args*"
        command(f'DumpGPU -path="{folder.as_posix()}/" -unified_filter="{filters}"')
    if dump and 55 <= self.frame < 61:
        self.capture.capture_scene()
    return original(self)

rig["Rig"].step = step
