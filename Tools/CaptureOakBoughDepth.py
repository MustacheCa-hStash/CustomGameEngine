"""Read linear scene depth in cm over the exposed bough at the 100 m match pose."""
import array
import json
import os
from pathlib import Path
import runpy
import unreal as u

out = Path(os.environ["OAK_CUT_OUTPUT"])
stage = Path(u.Paths.convert_relative_path_to_full(u.Paths.project_dir()))
jobs = out / "depth-jobs.json"
jobs.write_text(json.dumps([dict(asset="VoxelBough7", mesh_path="/Game/NaniteOakBench/AssemblyStep07JointVoxels",
    resolution=[3840,2160], vertical_fov=45, isolated=True, capture_only=True, frustum=1, hzb=0,
    distance=100, pitch=-2.235, yaw=132, name="vx7-depth", mode="Depth")]))
os.environ["OAK_CAPTURE_JOBS"] = str(jobs)
rig = runpy.run_path(str(stage / "capture_unreal.py"))

def capture(self):
    if self.i:
        return True
    if self.entry is None:
        self.set_job(rig["jobs"][0])
        self.target = u.RenderingLibrary.create_render_target2d(self.world,3840,2160,u.TextureRenderTargetFormat.RTF_RGBA32F)
        self.capture.set_editor_properties(dict(texture_target=self.target,capture_source=u.SceneCaptureSource.SCS_SCENE_DEPTH))
    self.capture.capture_scene()
    self.frame += 1
    if self.frame == 110:
        l,t,r,b = 1480,850,2040,1270
        values = u.RenderingLibrary.read_render_target_raw_pixel_area(self.world,self.target,l,t,r-l,b-t,False)
        assert len(values) == (r-l)*(b-t), len(values)
        with (out/"ue-depth-m.f32").open("wb") as f:
            array.array("f",(v.r*.01 for v in values)).tofile(f)
        self.i += 1
    return False

rig["Rig"].step = capture
