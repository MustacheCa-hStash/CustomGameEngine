"""Capture matched one- and four-oak Nanite triangle references in the staged UE project."""
import json
import math
import os
from pathlib import Path
import runpy

import unreal

stage = Path(unreal.Paths.convert_relative_path_to_full(unreal.Paths.project_dir()))
out = stage.parent / "oak-gpu-traversal"
out.mkdir(exist_ok=True)
jobs = []
for trees in (1, 4):
    for distance, pitch in ((24, -5.46), (41, -5.46), (100, -2.235)):
        base = dict(asset="Tree_Optimized_Triangles", mesh_path="/Game/NaniteOakBench/Tree_Optimized_Triangles",
                    resolution=[3840, 2160], vertical_fov=45, isolated=True, capture_only=True,
                    frustum=1, hzb=0, trees=trees, distance=distance, pitch=pitch, hold_frames=75)
        stem = f"gpu-oak-{trees}-{distance}"
        for yaw in (132, 23, 197):
            for error in (1, .001):
                jobs.append(dict(base, name=f"{stem}-{yaw}-{error}", yaw=yaw, max_pixels_per_edge=error, mode="Mask"))
        jobs.append(dict(base, name=stem+"-stats", yaw=132, mode="OffscreenStats"))
        for repeat in range(2):
            jobs.append(dict(base, name=f"{stem}-profile-{repeat}", yaw=132, mode="Profile"))
if os.environ.get("OAK_GROVE_CAPTURE_JOBS"):
    job_file = Path(os.environ["OAK_GROVE_CAPTURE_JOBS"])
else:
    job_file = out / "ue-jobs.json"
    job_file.write_text(json.dumps(jobs))
os.environ["OAK_CAPTURE_JOBS"] = str(job_file)
rig = runpy.run_path(str(stage / "capture_unreal.py"))
rig["command"]("r.ShadowQuality 0")
original = rig["Rig"].set_job
source = rig["source"]
spacing = 2 + max(source["maximum_m"][axis]-source["minimum_m"][axis] for axis in (0, 2))


def set_job(self, job):
    original(self, job)
    if not hasattr(self, "grove"):
        self.grove = []
        for i, (col, row) in enumerate(((-1, -1), (0, -1), (-1, 0))):
            actor = self.spawn(unreal.StaticMeshActor, rig["ue_position"]((col*spacing, 0, row*spacing)),
                               unreal.Rotator(yaw=-math.degrees(1.25*i)))
            scale = .92 + .04*((i*7) % 5)
            actor.set_actor_scale3d(unreal.Vector(scale, scale, scale))
            actor.static_mesh_component.set_visible_in_scene_capture_only(True)
            self.grove.append(actor)
    for actor in self.grove:
        mesh = self.meshes[job["asset"]]
        if actor.static_mesh_component.get_editor_property("static_mesh") != mesh:
            actor.static_mesh_component.set_static_mesh(mesh)
        actor.static_mesh_component.set_visibility(job["trees"] == 4, False)


rig["Rig"].set_job = set_job
