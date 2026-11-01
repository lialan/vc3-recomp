#!/usr/bin/env python3
"""Build an isolated direct-Vulkan VC3 APK; never installs or touches a device."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import zipfile

ROOT = Path(__file__).resolve().parents[1]
PACKAGE = "org.vc3recomp.vulkan.direct"


def run(args, **kwargs):
    return subprocess.run([str(x) for x in args], check=True, **kwargs)


def output(args):
    return subprocess.check_output([str(x) for x in args], text=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=Path, default=ROOT / "build-quest-vulkan-direct")
    parser.add_argument("--sdk", type=Path, default=Path.home() / "Library/Android/sdk")
    parser.add_argument("--deps", type=Path, default=ROOT.parent / "deps")
    parser.add_argument("--ndk", type=Path)
    parser.add_argument("--jobs", type=int, default=4)
    parser.add_argument("--gen-opt", default="-O1")
    args = parser.parse_args()
    build = args.build_dir.resolve()
    sdk, deps = args.sdk.resolve(), args.deps.resolve()
    ndk = (args.ndk or sdk / "ndk/27.3.13750724").resolve()
    host = "darwin-x86_64" if sys.platform == "darwin" else "linux-x86_64"
    llvm = ndk / "toolchains/llvm/prebuilt" / host / "bin"
    aapts = sorted((sdk / "build-tools").glob("*/aapt"))
    if not aapts:
        parser.error("Android build-tools aapt is required to verify the package ID")
    run(["cmake", "-S", ROOT, "-B", build, "-G", "Ninja",
         "-DCMAKE_TOOLCHAIN_FILE=" + str(ndk / "build/cmake/android.toolchain.cmake"),
         "-DANDROID_ABI=arm64-v8a", "-DANDROID_PLATFORM=android-29",
         "-DCMAKE_BUILD_TYPE=Release", "-DCHIHIRO_GAMES=vc3", "-DCHIHIRO_RENDERER=vulkan",
         "-DCHIHIRO_GEN_OPT=" + args.gen_opt,
         "-DSDL2_DIR=" + str(deps / "sdl2-android/lib/cmake/SDL2"),
         "-DOPENXR_DIR=" + str(deps / "oxr/prefab/modules/headers/include"),
         "-DOPENXR_LIB=" + str(deps / "oxr/jni/arm64-v8a/libopenxr_loader.so")])
    run(["cmake", "--build", build, "-j", args.jobs])
    native = build / "libmain.so"
    imports = output([llvm / "llvm-nm", "-u", native])
    dynamic = output([llvm / "llvm-readelf", "-d", native])
    if re.search(r"\b(?:gl[A-Z]|egl[A-Z]|SDL_GL_)", imports) or re.search(r"lib(?:EGL|GLES|GL\.|epoxy)", dynamic):
        raise RuntimeError("Native Vulkan libmain.so contains OpenGL imports/dependencies")
    libs = build / "apk-libs/arm64-v8a"
    libs.mkdir(parents=True, exist_ok=True)
    run([llvm / "llvm-strip", "--strip-unneeded", "-o", libs / "libmain.so", native])
    for source in [deps / "sdl2-android/lib/libSDL2.so", deps / "oxr/jni/arm64-v8a/libopenxr_loader.so"]:
        shutil.copy2(source, libs)
    # Include only the three explicitly staged libraries, never source-tree
    # leftovers from another backend. Default OpenGL packaging stays unchanged.
    if {p.name for p in libs.iterdir()} != {"libmain.so", "libSDL2.so", "libopenxr_loader.so"}:
        raise RuntimeError("Unexpected libraries in isolated APK staging directory")
    sdl_java = ROOT / "quest/vc3/src/main/java/org/libsdl"
    if not sdl_java.exists():
        shutil.copytree(deps / "SDL2-2.32.10/android-project/app/src/main/java/org/libsdl", sdl_java)
    env = dict(os.environ)
    env["ANDROID_HOME"] = str(sdk)
    java = Path("/opt/homebrew/opt/openjdk@21/libexec/openjdk.jdk/Contents/Home")
    if "JAVA_HOME" not in env and java.exists():
        env["JAVA_HOME"] = str(java)
    run([ROOT / "quest/gradlew", "-q", ":vc3:assembleRelease", "-PappId=" + PACKAGE,
         "-PappNamespace=org.vc3recomp", "-PappLabel=VC3 Vulkan Direct", "-PrequireGles=false",
         "-PnativeLibsDir=" + str(libs.parent)], cwd=ROOT / "quest", env=env)
    source = ROOT / "quest/vc3/build/outputs/apk/release/vc3-release.apk"
    badging = output([aapts[-1], "dump", "badging", source])
    if not badging.startswith("package: name='" + PACKAGE + "'"):
        raise RuntimeError("Package ID mismatch; refusing to produce a candidate APK")
    with zipfile.ZipFile(source) as apk:
        if apk.read("lib/arm64-v8a/libmain.so") != (libs / "libmain.so").read_bytes():
            raise RuntimeError("APK contains a stale native renderer")
    destination = build / "VC3-Vulkan-Direct.apk"
    shutil.copy2(source, destination)
    report = {"package": PACKAGE, "apk": str(destination),
              "sha256": hashlib.sha256(destination.read_bytes()).hexdigest(),
              "native_sha256": hashlib.sha256(native.read_bytes()).hexdigest(),
              "gl_imports": [], "dynamic_dependencies": dynamic,
              "badging": badging, "scope": "Build/import audit only; no headset validation or installation"}
    (build / "apk-report.json").write_text(json.dumps(report, indent=2) + "\n")
    print(destination)
    print("SHA256:", report["sha256"])


if __name__ == "__main__":
    main()
