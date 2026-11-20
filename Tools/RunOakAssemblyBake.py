"""Execute the offline reducer arms in a project-local Unreal editor module."""
import json
import hashlib
import os
from pathlib import Path
import unreal

world = unreal.get_editor_subsystem(unreal.UnrealEditorSubsystem).get_editor_world()
project = Path(unreal.Paths.convert_relative_path_to_full(unreal.Paths.project_dir()))
root = project.parent / "oak-assembly-step"
plugin = project/"Plugins/OakAssemblyBench/Binaries/Win64/UnrealEditor-OakAssemblyBench.dll"

def fingerprint(path):
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024*1024), b""):
            digest.update(block)
    return digest.hexdigest()

try:
    binary_hash = fingerprint(plugin)
    jobs = json.loads(Path(os.environ["OAK_BAKE_JOBS"]).read_text()) if "OAK_BAKE_JOBS" in os.environ else [
        {"input":"wood.mesh","arm":arm,"ratio":ratio,"output":f"{arm}-{ratio}"}
        for ratio in (.5,.68) for arm in ("epic","custom")]
    for job in jobs:
        dest = root / job["output"]
        source = root/job["input"]
        materials = source.with_suffix(".mat")
        signature = dict(job=job, input=fingerprint(source), module=binary_hash,
                         materials=fingerprint(materials) if materials.exists() else None)
        record = dest/"bake.json"
        done = dest/"done.txt"
        if os.environ.get("OAK_BAKE_FORCE") != "1" and done.exists() and record.exists() and json.loads(record.read_text()) == signature:
            continue
        done.unlink(missing_ok=True)
        unreal.SystemLibrary.execute_console_command(world, 'oak.Bake "{}" "{}" {} {}'.format(
            source.as_posix(),dest.as_posix(),job["arm"],job["ratio"]))
        if not done.exists():
            raise RuntimeError(f"Offline bake failed for {job}")
        record.write_text(json.dumps(signature,indent=2))
        unreal.log(f"OAK_BENCH completed {job}")
    unreal.log("OAK_BENCH ASSEMBLIES_COMPLETE")
finally:
    unreal.SystemLibrary.quit_editor()
