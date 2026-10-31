#!/usr/bin/env bash
# Lift vc3.xbe to C in games/vc3/generated (after scripts/analyze.sh).
# VC3_LIFT_FLAGS: the extra tools.recomp options; unset, --reg-cache (guest
# registers in C locals). VC3_LIFT_FLAGS= (empty) lifts without it.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
XBE="${1:-${VC3_GAME_DIR:-$HOME/Downloads/VR/VC3/extracted}/vc3.xbe}"
OUT="$ROOT/analysis/out"
mkdir -p "$ROOT/games/vc3/generated"
cd "$ROOT/external/xboxrecomp"
"$ROOT/.venv/bin/python" -m tools.recomp "$XBE" --all --split 250 --game-name vc3 \
    --gen-dir "$ROOT/games/vc3/generated" --disasm-dir "$OUT/disasm" \
    --func-id-dir "$OUT/func_id" --abi-dir "$OUT/abi" \
    --exclude-manual "$ROOT/games/vc3/src/recomp_manual.c" \
    ${VC3_TRACE:+--trace-functions "$VC3_TRACE"} ${VC3_LIFT_FLAGS---reg-cache}
