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
    # the rifle's model, embedded as the runtime embeds it (vk_rifle_assets.S)
    cc -O1 -I"$ROOT/runtime/src/host" -I"$ROOT/runtime/src/xr" \
        -DRIFLE_M700_MESH="\"$ROOT/third_party/rifle/m700.mesh\"" \
        -DRIFLE_M700_JPG="\"$ROOT/third_party/rifle/m700_color.jpg\"" \
        "${src[@]}" -o "$bin" -lm
    "$bin"
}
run tests/vr_rifle_test.c runtime/src/host/vr_rifle.c runtime/src/gpu/vulkan/vk_rifle_assets.S runtime/src/gpu/vulkan/vk_jpeg.c
run tests/retail_pad_test.c runtime/src/host/retail_pad.c
