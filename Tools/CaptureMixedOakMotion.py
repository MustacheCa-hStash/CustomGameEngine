"""Run both native representations on the identical continuous out-and-back group camera path."""
import os
import runpy
from pathlib import Path
import unreal as u

stage=Path(u.Paths.convert_relative_path_to_full(u.Paths.project_dir()))
destination=Path(os.environ["OAK_MIXED_DIR"])/"native"
destination.mkdir(exist_ok=True)
if os.environ.get("OAK_GROVE_CAPTURE_JOBS"):
    rig=runpy.run_path(os.environ["OAK_GROVE_MOTION_RIG"])["rig"]
else:
    rig=runpy.run_path(str(stage/"capture_unreal.py"))
rig["command"]("r.ShadowQuality 0")
original=rig["Rig"].step


def moving_step(self):
    if self.entry is None:
        return original(self)
    job=self.entry
    self.frame+=1
    warm=40
    frame=self.frame-warm
    steps=job["steps"]
    count=2*steps
    profile=job["mode"]=="Profile"
    if frame >= count+(8 if profile else 0):
        rig["log"]("Captured "+job["name"])
        self.i+=1
        self.entry=None
        return False
    # Warm at the first pose, then issue one capture at every position in each direction.
    pos=max(0,min(frame,count-1))
    k=pos if pos<steps else count-1-pos
    start=job.get("distance",50)
    d=start+(job.get("end_distance",100)-start)*k/(steps-1)
    if "pixel_schedule" in job:
        budget=job["pixel_schedule"][k]
        if frame==-warm+1 or budget!=getattr(self,"motion_budget",None):
            rig["command"](f"r.Nanite.MaxPixelsPerEdge {budget}")
            self.motion_budget=budget
    location,rotation=rig["camera_pose"](d,job["yaw"],job["pitch"])
    self.camera.set_actor_location_and_rotation(location,rotation,False,True)
    self.capture.capture_scene()
    if profile and frame == -3:
        rig["command"]("CsvProfile STARTFILE="+job["name"]+".csv")
        rig["command"](f"CsvProfile FRAMES={count+5}")
    if not profile and frame>=0:
        u.RenderingLibrary.export_render_target(self.world,self.capture.get_editor_property("texture_target"),
            str(destination),f'{job["name"]}-{frame:03}.png')
    return False


rig["Rig"].step=moving_step
