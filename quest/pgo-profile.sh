#!/usr/bin/env bash
# A PGO profile for the Quest build, made on the Mac.
#
#   quest/pgo-profile.sh [out.profdata]     (default quest/vc3-pgo.profdata)
#   VC3_QUEST_PGO=use:quest/vc3-pgo.profdata quest/build.sh
#
# Clang's source-based profiles count the C as written, so a profile taken
# from an instrumented Mac build of the same sources (the same lifted code)
# fits the Quest build. Two scripted runs, muted: attract -> Mission Select
# -> intro -> combat -> E.S. mode (pedal held), and the same in immersive
# 2x stereo. Apple's clang writes a newer profile format than the NDK's
# reads, so the merged profile goes through llvm-profdata's text form into
# the NDK's own llvm-profdata. Re-run it after a re-lift or a change to the
# runtime; out-of-date functions are just left unoptimised.
#
# Measured so far (one Mac run each): libmain.so 1,805,734 -> 1,570,712
# instructions, 47.9 -> 42.2 MB, 255 of 256 functions in one lifted file
# matched; no measurable change on the Mac's game thread or executor. On
# the headset, untried (devtest.sh has no build-time A/B: compare two APKs).
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OUT="${1:-$ROOT/quest/vc3-pgo.profdata}"
SDK=${ANDROID_HOME:-$HOME/Library/Android/sdk}
NDK=${ANDROID_NDK:-$SDK/ndk/27.3.13750724}
NDK_PROFDATA="$NDK/toolchains/llvm/prebuilt/darwin-x86_64/bin/llvm-profdata"
B="$ROOT/build-pgo-gen"
W="$(mktemp -d)"
trap 'rm -rf "$W"' EXIT

cmake -S "$ROOT" -B "$B" -G Ninja -DCMAKE_C_FLAGS=-fprofile-instr-generate \
    -DCMAKE_OBJC_FLAGS=-fprofile-instr-generate \
    -DCMAKE_EXE_LINKER_FLAGS=-fprofile-instr-generate >/dev/null
cmake --build "$B" -j"$(sysctl -n hw.ncpu)"

KEYS="30:coin,31:start,40:fire,44:fire,46:fire"
for t in 48 50 52 54 56 58 60 62 65 66 67 68 69 70 71 72 73 74; do KEYS="$KEYS,$t:fire"; done
for t in 64 64.5 65 65.5 66 66.5 67 67.5 68 68.5 69 69.5 70 70.5 71 71.5 72 72.5 73 73.5 74 74.5; do
    KEYS="$KEYS,$t:pedal"
done
run() {   # continuous mode (%c): the counts survive the run being killed at its end
    LLVM_PROFILE_FILE="$W/vc3-%c%p.profraw" VC3_BIN="$B/vc3" VC3_AUTOKEYS="$KEYS" \
        VC3_AUTOAIM=0.5,0.5 VC3_SAVE_DIR="$W/save" VC3_CONFIG_DIR="$W/save" "$@" \
        "$ROOT/scripts/run-test.sh" 82 "$W/run.log" >/dev/null
}
run env
run env VC3_STEREO=1 VC3_IMMERSIVE_TEST=1 VC3_IMMERSIVE_TEST_SCALE=2

xcrun llvm-profdata merge -o "$W/mac.profdata" "$W"/*.profraw
xcrun llvm-profdata merge --text -o "$W/vc3.proftext" "$W/mac.profdata"
"$NDK_PROFDATA" merge -o "$OUT" "$W/vc3.proftext"
"$NDK_PROFDATA" show "$OUT" | tail -3
echo "profile: $OUT  (VC3_QUEST_PGO=use:$OUT quest/build.sh)"
