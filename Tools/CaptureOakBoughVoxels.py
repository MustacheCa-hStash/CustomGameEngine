"""Capture the actual voxelized bough, including selected GPU cluster and brick buffers."""
import json
import os
from pathlib import Path
import runpy
import uuid
import unreal

root=Path(os.environ["OAK_CUT_OUTPUT"])
stage=Path(unreal.Paths.convert_relative_path_to_full(unreal.Paths.project_dir()))
jobs=[]
base=dict(asset="VoxelBough7",mesh_path="/Game/NaniteOakBench/AssemblyStep07JointVoxels",resolution=[3840,2160],
          vertical_fov=45,isolated=True,capture_only=True,frustum=1,hzb=0,hold_frames=100)
for d in (100,41,50,52,56,58):
    for mode in ("Mask","Fine","Voxels","OffscreenStats"):
        jobs.append(dict(base,name=f"vx7-{d}-{mode}",distance=d,pitch=-2.235 if d==100 else -5.46,yaw=132,
                         mode="Mask" if mode=="Fine" else mode,max_pixels_per_edge=.001 if mode=="Fine" else 1))
for y in (90,180,45,225,0,270,68,315,302,330,17,204):
    for mode in ("Mask","Fine"):
        jobs.append(dict(base,name=f"vx7-100-y{y}-{mode}",distance=100,pitch=-2.235,yaw=y,mode="Mask",
                         max_pixels_per_edge=.001 if mode=="Fine" else 1))
for i in range(4):
    jobs.append(dict(base,name=f"vx7-shaded-100-{i}",distance=100,pitch=-2.235,yaw=132,mode="Profile"))
path=root/"jobs.json";path.write_text(json.dumps(jobs,indent=2))
os.environ["OAK_CAPTURE_JOBS"]=str(path)
rig=runpy.run_path(str(stage/"capture_unreal.py"))
generation=uuid.uuid4().hex[:12]
step=rig["Rig"].step
cmd=rig["command"]
for c in ("r.DumpGPU.Texture 0","r.DumpGPU.Buffer 2","r.DumpGPU.Explore 0","r.DumpGPU.Upload 0",
          "r.DumpGPU.ConsoleVariables 0","r.DumpGPU.PassParameters 1"):
    cmd(c)
def capture(self):
    j=self.entry
    if j and j["mode"]=="OffscreenStats":
        folder=root/f'dump{j["distance"]}-{generation}'
        if self.frame==60:
            filters="|".join(f"*Rasterize*@{b}" for b in ("*VisibleClustersSWHW","*ClusterPageData","*AssemblyTransforms","*Raster*Args*"))
            filters+="|*CalculateSafeRasterizerArgs*@*Raster*Args*"
            cmd('DumpGPU -path="'+folder.as_posix()+'/" -unified_filter="'+filters+'"')
        if 60<=self.frame<65:self.capture.capture_scene()
        if self.frame==99:
            assert (folder/"Base/ResourceDescs.json").is_file(), folder
            (root/f'dump-{j["distance"]}-path.json').write_text(json.dumps(folder.name))
    return step(self)
rig["Rig"].step=capture
