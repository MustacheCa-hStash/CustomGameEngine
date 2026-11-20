"""Independent Preserve Area references for the two fresh bough orbit views."""
import json
import os
from pathlib import Path
import runpy
import unreal as u

out=Path(os.environ["OAK_CUT_OUTPUT"])
stage=Path(u.Paths.convert_relative_path_to_full(u.Paths.project_dir()))
jobs=[]
for yaw in (17,204):
    for fine in (False,True):
        jobs.append(dict(asset="Bough7",mesh_path="/Game/NaniteOakBench/AssemblyStep07Joint",
            name=f"joint7-cut-100-yaw{yaw}"+("-fine" if fine else ""),mode="Mask",distance=100,yaw=yaw,
            pitch=-2.235,resolution=[3840,2160],vertical_fov=45,hold_frames=100,frustum=1,hzb=0,
            isolated=True,capture_only=True,max_pixels_per_edge=.001 if fine else 1))
path=out/"holdouts.json";path.write_text(json.dumps(jobs))
os.environ["OAK_CAPTURE_JOBS"]=str(path)
runpy.run_path(str(stage/"capture_unreal.py"))
