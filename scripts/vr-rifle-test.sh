#!/usr/bin/env bash
# scripts/vr-rifle-test.sh: builds and runs each VR rifle test program in turn;
# fails if any fails. Add a test: one more run line at the bottom.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
run() {  # run <test.c> [more sources...], paths relative to the repo root
    local name bin src=()
    name="$(basename "$1" .c)"
    bin="${TMPDIR:-/tmp}/$name"
    for f in "$@"; do src+=("$ROOT/$f"); done
    cc -O1 -I"$ROOT/runtime/src/host" -I"$ROOT/runtime/src/xr" "${src[@]}" -o "$bin" -lm
    "$bin"
}
run tests/vr_rifle_test.c runtime/src/host/vr_rifle.c
run tests/retail_pad_test.c runtime/src/host/retail_pad.c
