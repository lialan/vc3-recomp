#!/usr/bin/env bash
# scripts/vr-math-test.sh [id]: tests/vr_math_test.c against a game's
# projection (games/<id>/game.conf TEST_PROJ, TEST_SCREEN_H; default vc3).
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
. "$ROOT/scripts/game.sh" "$@"
BIN="${TMPDIR:-/tmp}/vr_math_test_$GAME"
cc -O1 -I"$ROOT/runtime/src/host" ${TEST_PROJ:+"-DTEST_PROJ=$TEST_PROJ"} \
    ${TEST_SCREEN_H:+"-DTEST_SCREEN_H=$TEST_SCREEN_H"} \
    "$ROOT/tests/vr_math_test.c" -o "$BIN" -lm
"$BIN"
