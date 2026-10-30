#!/usr/bin/env bash
# Convert RECOMP_FB_DUMP frames to small PNGs next to them:
#   scripts/frames-to-png.sh <dir> [max_side] [every_nth]
set -euo pipefail
DIR="$1"
SIDE="${2:-384}"
STEP="${3:-1}"
i=0
for f in "$DIR"/*.bmp; do
    if (( i % STEP == 0 )); then
        sips -s format png -Z "$SIDE" "$f" --out "${f%.bmp}.png" >/dev/null 2>&1
    fi
    i=$((i + 1))
done
ls -l "$DIR"/*.png | awk '{print $5, $NF}'
