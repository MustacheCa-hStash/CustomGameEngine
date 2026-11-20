"""Read back the resident native triangle hierarchies and capture fresh validation poses."""
import json
import os
from pathlib import Path
import runpy
import unreal

stage=Path(unreal.Paths.convert_relative_path_to_full(unreal.Paths.project_dir()))
out=Path(os.environ.get("OAK_LIVE_NATIVE_DATA",str(stage.parent/"oak-live-selection")))
out.mkdir(parents=True,exist_ok=True)
jobs=[]
extra=os.environ.get("OAK_SELECTOR_EXTRA")=="1"
for subject in (("crown",) if extra else ("7","8","crown")):
    asset="Tree_Optimized_Triangles" if subject=="crown" else f"AssemblyStep{int(subject):02d}Joint"
    base=dict(asset=asset,mesh_path="/Game/NaniteOakBench/"+asset,resolution=[3840,2160],vertical_fov=45,
              isolated=True,capture_only=True,frustum=1,hzb=0,hold_frames=100,mode="Mask")
    for d in (41,100):
        views=(90,180,45,225,0,270,68,315,302,330)
        for y in (views if extra else (132,23,197)+ (views if subject=="crown" else ())):
            for fine in (True,False):
                jobs.append(dict(base,name=f"selector-{subject}-{d}-{y}"+("-fine" if fine else ""),
                    distance=d,pitch=-5.46 if d==41 else -2.235,yaw=y,max_pixels_per_edge=.001 if fine else 1))
path=out/("native-extra-jobs.json" if extra else "native-jobs.json");path.write_text(json.dumps(jobs))
os.environ["OAK_CAPTURE_JOBS"]=str(path)
rig=runpy.run_path(str(stage/"capture_unreal.py"))
cmd=rig["command"];step=rig["Rig"].step
for setting in ("r.DumpGPU.Texture 0","r.DumpGPU.Buffer 2","r.DumpGPU.Explore 0","r.DumpGPU.Upload 0",
                "r.DumpGPU.ConsoleVariables 0","r.DumpGPU.PassParameters 1","r.ShadowQuality 0"):
    cmd(setting)
def capture(self):
    j=self.entry
    if j and j["distance"]==41 and j["yaw"]==132 and j["max_pixels_per_edge"]==1:
        subject=j["name"].split("-")[1]
        if self.frame==55:
            layout=out/f"layout-{subject}.txt"
            cmd(f'oak.ExportLayout {j["mesh_path"]}.{j["asset"]} "{layout.as_posix()}"')
            assert layout.exists(),layout
            filters="*Cull*Hierarchy*@*Hierarchy*|*Cull*Node*@*Hierarchy*|*Cull*Cluster*@*Hierarchy*|*Node*Cull*@*Hierarchy*|*@*StreamingManager.Hierarchy"
            filters+="|*SW Rasterize (Triangles)*@*ClusterPageData|*SW Rasterize (Triangles)*@*VisibleClustersSWHW|*SW Rasterize (Triangles)*@*AssemblyTransforms|*CalculateSafeRasterizerArgs*@*Raster*Args*"
            folder=out/("dump-"+subject)
            cmd(f'DumpGPU -path="{folder.as_posix()}/" -unified_filter="{filters}"')
        if 55<=self.frame<61:self.capture.capture_scene()
    return step(self)
rig["Rig"].step=capture
