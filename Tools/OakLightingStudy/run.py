"""Stage, view and capture the private four-oak lighting study."""
import argparse
import filecmp
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile


HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
OUT = ROOT / "build/oak-lighting-study"
SCENE = OUT / "scene"
SOURCE = OUT / "source"
COMPILER = OUT / "compiler"
EXE = SCENE / "CustomGameEngine.exe"
BASELINE_ENV = "OAK_LIGHTING_STUDY_BASELINE"

# Orbit radius, yaw, pitch and displacement from the preset's focus in metres.
POSES = {
    "rim": (5, -78, -40, (0, 0, 5)),
    "under": (7, -37.24, -21, (0, 0, 0)),
    "grove": (24, 132, -5.46, (0, 0, 0)),
    "reverse": (24, 23, -5.46, (0, 0, 0)),
    "review": (41, 132, -5.46, (0, 0, 0)),
    "far": (100, 132, -2.235, (0, 0, 0)),
}


def freeze_scene():
    if SCENE.is_dir():
        return
    inputs = ROOT / "build/oak-live-selection"
    with tempfile.TemporaryDirectory(dir=OUT) as tmp:
        stage = Path(tmp) / "scene"
        shutil.copytree(inputs / "viewer-crown/assets", stage / "assets")
        shutil.copy2(inputs / "native-crown.oakcut", stage)
        shutil.copy2(ROOT / "Tools/OakGpuTraversalSettings.json", stage)
        stage.rename(SCENE)


def freeze_source():
    if SOURCE.is_dir():
        return
    with tempfile.TemporaryDirectory(dir=OUT) as tmp:
        stage = Path(tmp) / "source"
        for folder in ("src", "shaders", "third_party"):
            shutil.copytree(ROOT / folder, stage / folder)
        (stage / "Tools").mkdir()
        for pattern in ("*.cpp", "*.h"):
            for path in (ROOT / "Tools").glob(pattern):
                shutil.copy2(path, stage / "Tools")
        shutil.copy2(ROOT / "CMakeLists.txt", stage)
        # Keep Git from treating the snapshot as a subdirectory of the live checkout.
        env = dict(os.environ, GIT_CEILING_DIRECTORIES=str(stage.parent))
        subprocess.run(["git", "apply", "--no-index", "--ignore-space-change", HERE / "viewer.patch"],
                       check=True, cwd=stage, env=env)
        stage.rename(SOURCE)


def prepare(rebuild=False):
    OUT.mkdir(parents=True, exist_ok=True)
    freeze_scene()
    freeze_source()
    shader = HERE / "OakStudy.hlsli"
    staged_shader = SOURCE / "shaders" / shader.name
    changed = not staged_shader.exists() or not filecmp.cmp(shader, staged_shader, shallow=False)
    if changed:
        shutil.copyfile(shader, staged_shader)
    binary = COMPILER / "Release" / EXE.name
    if rebuild or changed or not binary.exists() or staged_shader.stat().st_mtime > binary.stat().st_mtime:
        cache = (ROOT / "build/CMakeCache.txt").read_text().splitlines()
        cmake = next(line.split("=", 1)[1] for line in cache if line.startswith("CMAKE_COMMAND:INTERNAL="))
        subprocess.run([cmake, "-S", SOURCE, "-B", COMPILER, "-G", "Visual Studio 17 2022", "-A", "x64"], check=True)
        subprocess.run([cmake, "--build", COMPILER, "--config", "Release", "--target", "CustomGameEngine",
                        "--parallel", "6"], check=True)
    if not EXE.exists() or not filecmp.cmp(binary, EXE, shallow=False):
        shutil.copy2(binary, EXE)


def flags(pose):
    distance, yaw, pitch, offset = POSES[pose]
    settings = json.loads((SCENE / "OakGpuTraversalSettings.json").read_text())
    budget = {41: 1.03649, 100: 0.49018}.get(distance, 1)
    camera = "under-canopy" if pose == "under" else "overview"
    args = ["--scene", "forked-oak", "--camera", camera, "--width", "3840", "--height", "2160",
            "--msaa", "1", "--tree-count", "4", "--distance", str(distance), "--yaw-degrees", str(yaw),
            "--pitch-degrees", str(pitch), "--wind-strength", "0", "--wind-time", "0", "--lighting", "combined",
            "--probe-gi", "bounce", "--cull-spray-wood-backfaces", "--lod", "auto", "--spray-mode", "hierarchy",
            "--tree-submit", "direct", "--spray-culling", "on", "--spray-local-lod", "on",
            "--spray-pixel-error", str(budget), "--live-cut", str(SCENE / "native-crown.oakcut"),
            "--live-cut-pixel-error", str(settings.get(f"4/{distance}/native", 1)), "--live-cut-gpu"]
    for axis, value in zip("xyz", offset):
        if value:
            args.extend([f"--focus-offset-{axis}", str(value)])
    return args


def run(pose, baseline=False, capture=None):
    env = os.environ.copy()
    env.pop(BASELINE_ENV, None)
    if baseline:
        env[BASELINE_ENV] = "1"
    if capture:
        capture.parent.mkdir(exist_ok=True, parents=True)
        subprocess.run([EXE, "--capture", capture.resolve(), *flags(pose), "--timing-samples", "8"],
                       cwd=SCENE, env=env, check=True)
    else:
        subprocess.Popen([EXE, "--review-capture", *flags(pose)], cwd=SCENE, env=env)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--pose", choices=POSES, default="rim")
    parser.add_argument("--baseline", action="store_true")
    parser.add_argument("--capture", type=Path)
    parser.add_argument("--rebuild", action="store_true")
    opts = parser.parse_args()
    prepare(opts.rebuild)
    run(opts.pose, opts.baseline, opts.capture)
