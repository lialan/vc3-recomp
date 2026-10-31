#!/usr/bin/env bash
# Run the xboxrecomp static-analysis passes on a game's XBE, into
# games/<id>/analysis (not committed).
#   scripts/analyze.sh [id] [path/to/game.xbe]   (id: a folder in games/, default vc3;
#                                                 the XBE: game.conf's, in the game's folder)
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
. "$ROOT/scripts/game.sh" "$@"
set -- ${GAME_ARGS[@]+"${GAME_ARGS[@]}"}
XBE_PATH="${1:-$GAME_DIR/$XBE}"
PY="$ROOT/.venv/bin/python"
OUT="$GAME_ROOT/analysis"
mkdir -p "$OUT"
cd "$ROOT/external/xboxrecomp"
"$PY" -m tools.xbe_parser "$XBE_PATH" --json "$OUT/xbe.json" --quiet
# Entry points static analysis misses (DPCs, thread starts); see the notes.
SEEDS="$GAME_ROOT/seed_functions.json"
"$PY" -m tools.disasm "$XBE_PATH" --analysis-json "$OUT/xbe.json" -o "$OUT/disasm" \
    ${SEEDS:+$( [ -f "$SEEDS" ] && echo --seed-functions "$SEEDS")} --force -v
"$PY" -m tools.func_id "$XBE_PATH" --functions "$OUT/disasm/functions.json" \
    --xrefs "$OUT/disasm/xrefs.json" --strings "$OUT/disasm/strings.json" \
    -o "$OUT/func_id" -v
"$PY" -m tools.abi_analysis "$XBE_PATH" --disasm-dir "$OUT/disasm" --func-id-dir "$OUT/func_id" \
    --output-dir "$OUT/abi" -v
