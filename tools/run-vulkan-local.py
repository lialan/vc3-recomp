#!/usr/bin/env python3
"""Bounded real-game Vulkan run with isolated settings and inspectable evidence.

Uses a local executable only. Does not connect to or install on any device.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import sys


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--executable", type=Path, required=True)
    parser.add_argument("--game", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--icd", type=Path)
    parser.add_argument("--frames", type=int, default=60)
    parser.add_argument("--timeout", type=int, default=60)
    parser.add_argument("--capture-first", type=int, default=0)
    parser.add_argument("--capture-count", type=int, default=3)
    parser.add_argument("--capture-every", type=int, default=0)
    args = parser.parse_args()
    if args.frames < 1 or args.timeout < 1 or args.capture_first < 0 or args.capture_count < 0 or args.capture_every < 0:
        parser.error("invalid frame/capture/timeout range")
    executable, game, output = (p.resolve() for p in (args.executable, args.game, args.output))
    if output.exists() and any(output.iterdir()):
        parser.error("output must be a fresh directory so stale captures cannot be reused")
    output.mkdir(parents=True, exist_ok=True)
    for subdir in ("frames", "save", "config"):
        (output / subdir).mkdir(exist_ok=True)
    env = dict(RECOMP_HEADLESS="1", RECOMP_FB_WINDOW="0", VC3_SOUND="0",
               VC3_SCALE="1", VC3_ASPECT="4:3", VC3_STEREO="0", VC3_FPS="60",
               VC3_SAVE_DIR=str(output / "save"), VC3_CONFIG_DIR=str(output / "config"),
               VC3_VULKAN_DUMP_DIR=str(output / "frames"),
               VC3_VULKAN_DUMP_FIRST=str(args.capture_first),
               VC3_VULKAN_DUMP_COUNT=str(args.capture_count), VC3_VULKAN_DUMP_EVERY=str(args.capture_every),
               VC3_VULKAN_FRAME_LIMIT=str(args.frames))
    if args.icd:
        env["VK_ICD_FILENAMES"] = str(args.icd.resolve())
    for key in ("VK_INSTANCE_LAYERS", "VK_LAYER_PATH", "VK_LOADER_DEBUG",
                "CHIHIRO_VULKAN_PIPELINE_CACHE", "VC3_VULKAN_PIPELINE_CACHE", "CHIHIRO_VULKAN_PROFILE", "CHIHIRO_VULKAN_FRAME_COSTS", "CHIHIRO_VULKAN_REUSE_CLEAN_DEPTH", "CHIHIRO_VULKAN_MULTIPLE_DEPTH", "CHIHIRO_VULKAN_SIZE_AWARE_EVICTION", "VC3_VULKAN_SIZE_AWARE_EVICTION", "CHIHIRO_VULKAN_PIPELINE_LOG", "CHIHIRO_VULKAN_RESIDENT_COLOR", "VC3_VULKAN_RESIDENT_COLOR",
                "CHIHIRO_VULKAN_GPU_MORTON_DEPTH", "VC3_VULKAN_GPU_MORTON_DEPTH", "CHIHIRO_VULKAN_GPU_MORTON_COLOR", "VC3_VULKAN_GPU_MORTON_COLOR", "CHIHIRO_VULKAN_GPU_COLOR_PACK", "VC3_VULKAN_GPU_COLOR_PACK", "CHIHIRO_VULKAN_GPU_TEXTURE_DECODE", "VC3_VULKAN_GPU_TEXTURE_DECODE", "CHIHIRO_VULKAN_GPU_DEPTH_PACK", "VC3_VULKAN_GPU_DEPTH_PACK", "CHIHIRO_VULKAN_DEPTH_QUANT_FRAGMENT", "VC3_VULKAN_DEPTH_QUANT_FRAGMENT", "CHIHIRO_VULKAN_DEPTH_QUANT_INLINE", "VC3_VULKAN_DEPTH_QUANT_INLINE", "CHIHIRO_VULKAN_DEPTH_QUANT_DRAW", "VC3_VULKAN_DEPTH_QUANT_DRAW", "CHIHIRO_VULKAN_QUANTIZED_DEPTH", "CHIHIRO_VULKAN_RESIDENT_DEPTH", "VC3_VULKAN_RESIDENT_DEPTH", "CHIHIRO_VULKAN_DEPTH_GPU_COPY", "VC3_VULKAN_DEPTH_GPU_COPY", "CHIHIRO_VULKAN_DEPTH_CLEAR_SKIP", "VC3_VULKAN_DEPTH_CLEAR_SKIP", "CHIHIRO_VULKAN_DEPTH_PACKED", "VC3_VULKAN_DEPTH_PACKED",
                "CHIHIRO_AUTOKEYS", "VC3_AUTOKEYS", "CHIHIRO_AUTOKEYS_CLOCK", "VC3_AUTOKEYS_CLOCK", "CHIHIRO_AUTOFIRE_MS", "VC3_AUTOFIRE_MS",
                "CHIHIRO_AUTOAIM", "VC3_AUTOAIM", "CHIHIRO_INPUT_SELFTEST", "VC3_INPUT_SELFTEST",
                "RECOMP_FRAME_TRACE", "RECOMP_PROF", "RECOMP_VSH_VERTEX_CACHE", "RECOMP_PB_DECODE_THREAD", "RECOMP_PB_WALK", "RECOMP_PB_SPLIT_LOG", "CHIHIRO_STRICT_GPU_FENCES", "VC3_STRICT_GPU_FENCES", "CHIHIRO_FENCE_LOG", "VC3_FENCE_LOG",
                "CHIHIRO_VULKAN_COLOR_FAST_IO", "VC3_VULKAN_COLOR_FAST_IO", "CHIHIRO_VULKAN_BATCH_LIMIT", "VC3_VULKAN_BATCH_LIMIT", "CHIHIRO_VULKAN_BATCH_DRAWS", "VC3_VULKAN_BATCH_DRAWS", "RECOMP_GPU_XF", "CHIHIRO_VULKAN_GPU_FIXED", "VC3_VULKAN_GPU_FIXED", "CHIHIRO_VULKAN_REUSE_RESOURCES", "CHIHIRO_VULKAN_FORCE_COMBINER", "CHIHIRO_VULKAN_TEXTURE_CACHE", "VC3_VULKAN_TEXTURE_CACHE",
                "CHIHIRO_VULKAN_TEXTURE_CACHE_ENTRIES", "VC3_VULKAN_TEXTURE_CACHE_ENTRIES",
                "CHIHIRO_VULKAN_TEXTURE_CACHE_MIB", "VC3_VULKAN_TEXTURE_CACHE_MIB",
                "CHIHIRO_VULKAN_TEXTURE_WRITE_WATCH", "VC3_VULKAN_TEXTURE_WRITE_WATCH", "CHIHIRO_VULKAN_WRITE_WATCH", "VC3_VULKAN_WRITE_WATCH",
                "CHIHIRO_VULKAN_NOAT", "VC3_VULKAN_NOAT", "CHIHIRO_VULKAN_GPU_TIMESTAMPS", "CHIHIRO_VULKAN_SPECIALIZE_RC",
                "CHIHIRO_VULKAN_GPU_RENDER_TEXTURES", "VC3_VULKAN_GPU_RENDER_TEXTURES"):
        if key in os.environ:
            env[key] = os.environ[key]
    report = dict(binary=str(executable), binary_sha256=hashlib.sha256(executable.read_bytes()).hexdigest(),
                  game=str(game), environment=env, requested_frames=args.frames,
                  scope="Local VC3 guest execution; no Quest or VR acceptance")
    if sys.platform == "darwin":
        libraries = subprocess.check_output(["otool", "-L", str(executable)], text=True)
        symbols = subprocess.check_output(["nm", "-u", str(executable)], text=True)
        (output / "libraries.txt").write_text(libraries)
        (output / "imports.txt").write_text(symbols)
        bad = [line for line in symbols.splitlines() if re.search(r"_(?:gl[A-Z]|egl[A-Z]|CGL|SDL_GL_)", line)]
        bad += [line for line in libraries.splitlines() if re.search(r"OpenGL|GLES|epoxy", line)]
        report["opengl_imports"] = bad
        if bad:
            raise SystemExit("FAIL: executable still imports OpenGL: " + repr(bad))
    with (output / "game.log").open("w") as log:
        try:
            process = subprocess.run([str(executable), str(game)], env={**os.environ, **env},
                                     stdout=log, stderr=subprocess.STDOUT, timeout=args.timeout)
            report["exit_code"] = process.returncode
        except subprocess.TimeoutExpired:
            report["exit_code"] = "timeout"
    text = (output / "game.log").read_text(errors="replace")
    # Keep individual windows: an overall average hides loading/scene changes.
    interval_pattern = re.compile(
        r"\[Vulkan frame intervals\] flips (\d+)\.\.(\d+): ([\d.]+) fps mean ([\d.]+)ms "
        r"p95 ([\d.]+)ms p99 ([\d.]+)ms max ([\d.]+)ms >20ms=(\d+) >33ms=(\d+)")
    report["frame_windows"] = [dict(
        first_flip=int(m[1]), last_flip=int(m[2]), fps=float(m[3]),
        mean_ms=float(m[4]), p95_ms=float(m[5]), p99_ms=float(m[6]), max_ms=float(m[7]),
        over_20ms=int(m[8]), over_33ms=int(m[9])) for m in interval_pattern.finditer(text)]
    report["failure_diagnostics"] = [line for line in text.splitlines()
        if "[Vulkan] fatal" in line or ("[D3D] BlockOnTime" in line and "timed out" in line)
        or "[PB] bad target" in line]
    report["reached_frame_limit"] = f"diagnostic frame limit reached: {args.frames}" in text
    report["captures"] = {p.name: hashlib.sha256(p.read_bytes()).hexdigest()
                          for p in sorted((output / "frames").glob("*.ppm"))}
    report["passed_execution"] = report["exit_code"] == 0 and report["reached_frame_limit"]
    (output / "report.json").write_text(json.dumps(report, indent=2) + "\n")
    print("\n".join(text.splitlines()[-30:]))
    print(f"Execution passed={report['passed_execution']}; evidence={output}")
    return 0 if report["passed_execution"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
