#!/usr/bin/env bash
# Run the xboxrecomp static-analysis passes on vc3.xbe.
# Usage: scripts/analyze.sh [path/to/vc3.xbe]   (default: $VC3_GAME_DIR/vc3.xbe)
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
XBE="${1:-${VC3_GAME_DIR:-$HOME/Downloads/VR/VC3/extracted}/vc3.xbe}"
PY="$ROOT/.venv/bin/python"
OUT="$ROOT/analysis/out"
mkdir -p "$OUT"
cd "$ROOT/external/xboxrecomp"
"$PY" -m tools.xbe_parser "$XBE" --json "$OUT/xbe.json" --quiet
# Entry points static analysis misses (DPCs, thread starts); see the notes.
SEEDS="$ROOT/analysis/seed_functions.json"
"$PY" -m tools.disasm "$XBE" --analysis-json "$OUT/xbe.json" -o "$OUT/disasm" \
    ${SEEDS:+$( [ -f "$SEEDS" ] && echo --seed-functions "$SEEDS")} --force -v
"$PY" -m tools.func_id "$XBE" --functions "$OUT/disasm/functions.json" \
    --xrefs "$OUT/disasm/xrefs.json" --strings "$OUT/disasm/strings.json" \
    -o "$OUT/func_id" -v
"$PY" -m tools.abi_analysis "$XBE" --disasm-dir "$OUT/disasm" --func-id-dir "$OUT/func_id" \
    --output-dir "$OUT/abi" -v
