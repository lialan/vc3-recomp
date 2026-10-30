#!/usr/bin/env bash
# Lift vc3.xbe to C in src/recomp/gen (after scripts/analyze.sh).
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
XBE="${1:-${VC3_GAME_DIR:-$HOME/Downloads/VR/VC3/extracted}/vc3.xbe}"
OUT="$ROOT/analysis/out"
mkdir -p "$ROOT/src/recomp/gen"
cd "$ROOT/external/xboxrecomp"
"$ROOT/.venv/bin/python" -m tools.recomp "$XBE" --all --split 250 --game-name vc3 \
    --gen-dir "$ROOT/src/recomp/gen" --disasm-dir "$OUT/disasm" \
    --func-id-dir "$OUT/func_id" --abi-dir "$OUT/abi" \
    --exclude-manual "$ROOT/src/recomp_manual.c"
