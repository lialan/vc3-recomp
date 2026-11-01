#!/usr/bin/env python3
"""Run through ~/.local/bin/quest3-lock for the ENTIRE session.

Actual Quest guest/Vulkan diagnostics without XR or installed-app changes.
This does not measure compositor, tracking, overlays or VR frame pacing.
"""
import argparse
import hashlib
import json
from pathlib import Path
import shlex
import subprocess
import time
import uuid


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--frames", type=int, default=4)
    parser.add_argument("--timeout", type=int, default=45)
    parser.add_argument("--capture-first", type=int, default=3)
    parser.add_argument("--capture-count", type=int, default=1)
    parser.add_argument("--capture-every", type=int, default=0, help="Periodic diagnostic captures; adds I/O to frame timings")
    parser.add_argument("--autokeys", default="")
    parser.add_argument("--game", default="/sdcard/VirtuaCop3")
    parser.add_argument("--build-dir", type=Path)
    parser.add_argument("--strict-depth-switches", action="store_true")
    parser.add_argument("--strict-depth-clears", action="store_true")
    parser.add_argument("--float-depth-transfers", action="store_true")
    parser.add_argument("--generic-color-transfers", action="store_true")
    parser.add_argument("--simpleperf", action="store_true", help="Profile only this workload with the bundled NDK tool; no system permission changes")
    args = parser.parse_args()
    if args.frames < 1 or not 1 <= args.timeout <= 3600 or args.capture_first < 0 or args.capture_count < 0 or args.capture_every < 0:
        parser.error("invalid frame/capture/timeout range")
    output = args.output.resolve()
    if output.exists() and any(output.iterdir()):
        parser.error("output must be a fresh directory")
    root = Path(__file__).resolve().parent.parent
    build, deps = (args.build_dir or root / "build-quest-vulkan-direct").resolve(), root.parent / "deps"
    files = [build / "vc3_vulkan_offscreen", build / "libmain.so",
             deps / "sdl2-android/lib/libSDL2.so", deps / "oxr/jni/arm64-v8a/libopenxr_loader.so"]
    if args.simpleperf:
        files.append(Path.home() / "Library/Android/sdk/ndk/27.3.13750724/simpleperf/bin/android/arm64/simpleperf")
    hashes = {p.name: hashlib.sha256(p.read_bytes()).hexdigest() for p in files}
    adb = [str(Path.home() / "Library/Android/sdk/platform-tools/adb"), "-s", "2G97C5ZJ0G02DQ"]
    remote = "/data/local/tmp/vc3-offscreen-" + uuid.uuid4().hex
    executable = remote + "/vc3_vulkan_offscreen"

    def run(argv, **kwargs):
        return subprocess.run(adb + argv, check=True, capture_output=True, timeout=20, **kwargs)

    def baseline():
        path = run(["shell", "pm", "path", "org.vc3recomp.vr"], text=True).stdout.strip().removeprefix("package:")
        return run(["shell", "sha256sum", path], text=True).stdout.split()[0]

    original = baseline()
    output.mkdir(parents=True, exist_ok=True)
    env = {"LD_LIBRARY_PATH": remote, "VC3_SAVE_DIR": remote + "/save",
           "VC3_CONFIG_DIR": remote + "/config", "VC3_SOUND": "0", "VC3_SCALE": "1",
           "VC3_FPS": "60", "VC3_ASPECT": "4:3", "VC3_STEREO": "0", "RECOMP_GPU_XF": "ff",
           "RECOMP_PB_DECODE_THREAD": "0", "CHIHIRO_VULKAN_FRAME_LIMIT": str(args.frames),
           "CHIHIRO_VULKAN_RESIDENT_DEPTH": "1", "CHIHIRO_VULKAN_PROFILE": "1",
           "CHIHIRO_VULKAN_DUMP_DIR": remote + "/frames",
           "CHIHIRO_VULKAN_DUMP_FIRST": str(args.capture_first),
           "CHIHIRO_VULKAN_DUMP_COUNT": str(args.capture_count), "CHIHIRO_AUTOKEYS_CLOCK": "flips",
           "CHIHIRO_AUTOKEYS": args.autokeys, "CHIHIRO_AUTOAIM": "0.2,0.5", "CHIHIRO_AUTOFIRE_MS": "700"}
    env["CHIHIRO_VULKAN_DEPTH_GPU_COPY"] = "0" if args.strict_depth_switches else "1"
    env["CHIHIRO_VULKAN_DEPTH_CLEAR_SKIP"] = "0" if args.strict_depth_clears else "1"
    env["CHIHIRO_VULKAN_DEPTH_PACKED"] = "0" if args.float_depth_transfers else "1"
    env["CHIHIRO_VULKAN_COLOR_FAST_IO"] = "0" if args.generic_color_transfers else "1"
    env["CHIHIRO_VULKAN_DUMP_EVERY"] = str(args.capture_every)
    report = {"serial": "2G97C5ZJ0G02DQ", "binary_hashes": hashes, "environment": env,
              "requested_frames": args.frames, "game": args.game,
              "scope": "Actual Quest guest/Vulkan; no XR session or VR performance acceptance"}
    report["profiling"] = "cpu-clock:u at199Hz with callchains" if args.simpleperf else None
    report["build_options"] = {line.split(":", 1)[0]: line.split("=", 1)[1]
        for line in (build / "CMakeCache.txt").read_text().splitlines()
        if line.split(":", 1)[0] in ("CHIHIRO_GEN_OPT", "CMAKE_C_FLAGS", "CMAKE_BUILD_TYPE") and "=" in line}
    start = time.monotonic()
    try:
        run(["shell", "mkdir", "-p", remote + "/save", remote + "/config", remote + "/frames"])
        for path in files:
            run(["push", str(path), remote + "/" + path.name])
        run(["shell", "chmod", "700", executable] + ([remote + "/simpleperf"] if args.simpleperf else []))
        inner = "echo $$ >" + shlex.quote(remote + "/pid") + "; exec " + shlex.join(
            ["env"] + [k + "=" + v for k, v in env.items()] + [executable, args.game])
        workload = ["sh", "-c", inner]
        if args.simpleperf:
            workload = [remote + "/simpleperf", "record", "-e", "cpu-clock:u", "-f", "199", "-g",
                        "-o", remote + "/perf.data", "--"] + workload
        command = shlex.join(["timeout", "-s", "KILL", str(args.timeout)] + workload)
        result = subprocess.run(adb + ["shell", command], capture_output=True, timeout=args.timeout + 20)
        text = (result.stdout + result.stderr).decode(errors="replace")
        (output / "game.log").write_text(text)
        run(["pull", remote + "/frames", str(output)])
        if args.simpleperf and result.returncode == 0:
            profile = run(["shell", shlex.join([remote + "/simpleperf", "report", "-i", remote + "/perf.data",
                                               "--sort", "dso,symbol"])], text=True)
            (output / "profile.txt").write_text(profile.stdout + profile.stderr)
            run(["pull", remote + "/perf.data", str(output / "perf.data")])
        report.update(exit_code=result.returncode,
                      reached_frame_limit=f"diagnostic frame limit reached: {args.frames}" in text)
        print("\n".join(text.splitlines()[-30:]))
    finally:
        # Only stop a still-live process whose command line identifies our UUID.
        pid = subprocess.run(adb + ["shell", "cat", remote + "/pid"], capture_output=True, timeout=20).stdout.strip()
        if pid.isdigit():
            cmdline = subprocess.run(adb + ["exec-out", "cat", "/proc/" + pid.decode() + "/cmdline"],
                                     capture_output=True, timeout=20).stdout
            if cmdline.split(b"\0", 1)[0] == executable.encode():
                run(["shell", "kill", "-KILL", pid.decode()])
        run(["shell", "rm", "-r", "--", remote])
        final = baseline()
        report.update(elapsed_seconds=time.monotonic() - start, baseline_before=original, baseline_after=final)
        (output / "report.json").write_text(json.dumps(report, indent=2) + "\n")
        if final != original:
            raise RuntimeError("Working APK hash changed during session")
        print("Session cleaned; original APK unchanged; evidence:", output)
    return 0 if report.get("exit_code") == 0 and report.get("reached_frame_limit") else 1


if __name__ == "__main__":
    raise SystemExit(main())
