#!/usr/bin/env bash
# Build a Chihiro game's Quest app (Quest 3 / 3S / 2 / Pro).
#
#   quest/build.sh [id]             -> quest/<APK>.apk (id: a folder in games/,
#                                      default vc3 -> quest/VirtuaCop3VR.apk)
#   quest/build.sh [id] --native    only libmain.so (build-quest/), no APK
#   APK_OUT=path quest/build.sh     the APK under another name
#
# The game's names come from games/<id>/game.conf, its app module is
# quest/<id>. The APK holds no game files: copy the game's XBE and media/ to
# the headset (quest/install.sh). Needs the lifted game in games/<id>/generated
# (scripts/lift.sh <id>), the Android SDK with NDK 27, a JDK 17+, and SDL2 and
# the OpenXR loader for arm64 Android in $DEPS (default ../deps beside the
# repository; the lindbergh-recomp-ports checkout's quest/deps.sh makes the
# same folder).
set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd)
. "$ROOT/scripts/game.sh" "$@"
set -- ${GAME_ARGS[@]+"${GAME_ARGS[@]}"}
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
[ -f "$Q/$GAME/build.gradle" ] || { echo "no quest/$GAME/build.gradle: the game's app module"; exit 1; }

# The Quest's cores: the instruction set the Quest 2 / Pro (XR2: Cortex-A77)
# and the Quest 3 / 3S (XR2 Gen 2: Cortex-A78 class) share, scheduled for the
# A78. CHIHIRO_QUEST_CFLAGS (or VC3_QUEST_CFLAGS) replaces it.
QCF=${CHIHIRO_QUEST_CFLAGS:-${VC3_QUEST_CFLAGS:-}}
CPU_FLAGS=${QCF:--march=armv8.2-a+fp16+dotprod -mtune=cortex-a78}
# Profile-guided optimisation (optional), CHIHIRO_QUEST_PGO (or VC3_QUEST_PGO):
#   use:<file>  a build optimised with that .profdata (the NDK's clang's
#               format; quest/pgo-profile.sh makes one from a Mac run of the
#               same sources)
#   gen         an instrumented build; it writes .profraw files under
#               <HEADSET_DIR>/pgo while it plays (merge with the NDK's
#               llvm-profdata)
PGO=${CHIHIRO_QUEST_PGO:-${VC3_QUEST_PGO:-}}
case "$PGO" in
    "")  ;;
    gen) CPU_FLAGS="$CPU_FLAGS -fprofile-generate=$HEADSET_DIR/pgo" ;;
    *)   PGO_FILE=${PGO#use:}
         [ -f "$PGO_FILE" ] || { echo "QUEST_PGO: no profile at $PGO_FILE"; exit 1; }
         CPU_FLAGS="$CPU_FLAGS -fprofile-instr-use=$PGO_FILE -Wno-profile-instr-unprofiled -Wno-profile-instr-out-of-date" ;;
esac

# Pipeline seeds are trained on the target Quest GPU/driver, not the build host.
# An unset variable preserves the CMake setting; explicitly empty clears it.
PIPELINE_ARGS=("-DCHIHIRO_VULKAN_PIPELINE_CACHE_VENDOR=0x5143")
if [ "${CHIHIRO_VULKAN_PIPELINE_CACHE_SEEDS+x}" = x ]; then
    PIPELINE_ARGS+=("-DCHIHIRO_VULKAN_PIPELINE_CACHE_SEEDS=$CHIHIRO_VULKAN_PIPELINE_CACHE_SEEDS")
else
    # Per-game Quest3 training artifacts; multiple driver UUIDs may coexist.
    PACKAGED_SEEDS=""
    for seed in "$ROOT/games/$GAME/pipeline-cache/quest3/"*.cache; do
        [ -f "$seed" ] || continue
        PACKAGED_SEEDS="${PACKAGED_SEEDS:+$PACKAGED_SEEDS;}$seed"
    done
    PIPELINE_ARGS+=("-DCHIHIRO_VULKAN_PIPELINE_CACHE_SEEDS=$PACKAGED_SEEDS")
fi
# Vulkan release packages require a seed; OFF is an explicit training build.
PIPELINE_ARGS+=("-DCHIHIRO_VULKAN_REQUIRE_PIPELINE_CACHE_SEEDS=${CHIHIRO_VULKAN_REQUIRE_PIPELINE_CACHE_SEEDS:-ON}")
if [ -n "${CHIHIRO_RENDERER:-}" ]; then
    PIPELINE_ARGS+=("-DCHIHIRO_RENDERER=$CHIHIRO_RENDERER")
fi

# one game per build tree (its libmain.so); vc3's is build-quest
if [ "$GAME" = vc3 ]; then B=$ROOT/build-quest; else B=$ROOT/build-quest-$GAME; fi
cmake -S "$ROOT" -B "$B" -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE="$NDK/build/cmake/android.toolchain.cmake" \
    -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-29 -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_C_FLAGS="$CPU_FLAGS" \
    -DCHIHIRO_GAMES="$GAME" "${PIPELINE_ARGS[@]}" \
    -DSDL2_DIR="$DEPS/sdl2-android/lib/cmake/SDL2" \
    -DOPENXR_DIR="$DEPS/oxr/prefab/modules/headers/include" \
    -DOPENXR_LIB="$DEPS/oxr/jni/arm64-v8a/libopenxr_loader.so" >/dev/null
cmake --build "$B" -j"$(getconf _NPROCESSORS_ONLN)" ${TARGET:+--target "$TARGET"}
[ "${1:-}" = --native ] && exit 0

M=$Q/$GAME
LIBS=$M/src/main/libs/arm64-v8a
mkdir -p "$LIBS"
"$STRIP" --strip-unneeded -o "$LIBS/libmain.so" "$B/libmain.so"
cp "$DEPS/sdl2-android/lib/libSDL2.so" "$DEPS/oxr/jni/arm64-v8a/libopenxr_loader.so" "$LIBS/"
rm -rf "$M/src/main/java/org/libsdl"
cp -R "$SDL_SRC/android-project/app/src/main/java/org/libsdl" "$M/src/main/java/org/"
echo "sdk.dir=$SDK" > "$Q/local.properties"

cd "$Q"
./gradlew -q ":$GAME:assembleRelease" -PappId="$PKG" -PappNamespace="$NAMESPACE"
OUT=${APK_OUT:-$Q/$APK.apk}      # APK_OUT: another name, leaving the installed one
cp "$GAME/build/outputs/apk/release/$GAME-release.apk" "$OUT"
echo "built $OUT"
