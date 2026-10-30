#!/bin/sh
# Build Virtua Cop 3 VR for Meta Quest (Quest 3 / 3S / 2 / Pro).
#
#   quest/build.sh              -> quest/VirtuaCop3VR.apk
#   quest/build.sh --native     only libmain.so (build-quest/), no APK
#
# The APK holds no game files: copy your vc3.xbe and media/ to the headset
# (quest/install.sh). Needs the lifted game in src/recomp/gen (scripts/lift.sh),
# the Android SDK with NDK 27, a JDK 17+, and SDL2 and the OpenXR loader for
# arm64 Android in $DEPS (default ../deps beside the repository; the
# lindbergh-recomp-ports checkout's quest/deps.sh makes the same folder).
set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd)
Q=$ROOT/quest
SDK=${ANDROID_SDK_ROOT:-$HOME/Library/Android/sdk}
NDK=${ANDROID_NDK:-$SDK/ndk/27.3.13750724}
DEPS=${DEPS:-$HOME/Downloads/VR/deps}
SDL_SRC=${SDL_SRC:-$DEPS/SDL2-2.32.10}
[ -n "$JAVA_HOME" ] || [ ! -d /opt/homebrew/opt/openjdk@21 ] || export JAVA_HOME=/opt/homebrew/opt/openjdk@21/libexec/openjdk.jdk/Contents/Home
case "$(uname -s)" in Darwin) HOST=darwin-x86_64 ;; *) HOST=linux-x86_64 ;; esac
STRIP=$NDK/toolchains/llvm/prebuilt/$HOST/bin/llvm-strip

for f in "$DEPS/sdl2-android/lib/libSDL2.so" "$DEPS/oxr/jni/arm64-v8a/libopenxr_loader.so"; do
    [ -f "$f" ] || { echo "missing $f: build the dependencies first (see the header)"; exit 1; }
done

B=$ROOT/build-quest
cmake -S "$ROOT" -B "$B" -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE="$NDK/build/cmake/android.toolchain.cmake" \
    -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-29 -DCMAKE_BUILD_TYPE=Release \
    -DSDL2_DIR="$DEPS/sdl2-android/lib/cmake/SDL2" \
    -DOPENXR_DIR="$DEPS/oxr/prefab/modules/headers/include" \
    -DOPENXR_LIB="$DEPS/oxr/jni/arm64-v8a/libopenxr_loader.so" >/dev/null
cmake --build "$B" -j"$(getconf _NPROCESSORS_ONLN)" ${TARGET:+--target "$TARGET"}
[ "$1" = --native ] && exit 0

M=$Q/vc3
LIBS=$M/src/main/libs/arm64-v8a
mkdir -p "$LIBS"
"$STRIP" --strip-unneeded -o "$LIBS/libmain.so" "$B/libmain.so"
cp "$DEPS/sdl2-android/lib/libSDL2.so" "$DEPS/oxr/jni/arm64-v8a/libopenxr_loader.so" "$LIBS/"
rm -rf "$M/src/main/java/org/libsdl"
cp -R "$SDL_SRC/android-project/app/src/main/java/org/libsdl" "$M/src/main/java/org/"
echo "sdk.dir=$SDK" > "$Q/local.properties"

cd "$Q"
./gradlew -q :vc3:assembleRelease
cp vc3/build/outputs/apk/release/vc3-release.apk "$Q/VirtuaCop3VR.apk"
echo "built $Q/VirtuaCop3VR.apk"
