#!/usr/bin/env bash
# Lift a game's XBE to C in games/<id>/generated (after scripts/analyze.sh).
#   scripts/lift.sh [id] [path/to/game.xbe]     (id: a folder in games/, default vc3)
# The XBE must be the one games/<id>/game.conf names (XBE_SHA256): the
# profile's addresses and recomp_manual.c are for that build.
# <PREFIX>_LIFT_FLAGS / CHIHIRO_LIFT_FLAGS: the extra tools.recomp options;
# unset, game.conf's LIFT_FLAGS (VC3: --reg-cache, guest registers in C
# locals); set empty, none.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
. "$ROOT/scripts/game.sh" "$@"
set -- ${GAME_ARGS[@]+"${GAME_ARGS[@]}"}
XBE_PATH="${1:-$GAME_DIR/$XBE}"
OUT="$GAME_ROOT/analysis"
if [ -n "${XBE_SHA256:-}" ]; then
    sum=$(shasum -a 256 "$XBE_PATH" | awk '{print $1}')
    if [ "$sum" != "$XBE_SHA256" ]; then
        echo "$XBE_PATH: SHA-256 $sum, not the $TITLE XBE games/$GAME/game.conf names ($XBE_SHA256)" >&2
        exit 1
    fi
fi
_lf="${ENV_PREFIX}_LIFT_FLAGS"
if [ -n "${CHIHIRO_LIFT_FLAGS+set}" ]; then FLAGS=$CHIHIRO_LIFT_FLAGS
elif [ -n "${!_lf+set}" ]; then FLAGS=${!_lf}
else FLAGS=${LIFT_FLAGS:-}; fi
_tr="${ENV_PREFIX}_TRACE"
TRACE="${CHIHIRO_TRACE:-${!_tr:-}}"
mkdir -p "$GAME_ROOT/generated"
cd "$ROOT/external/xboxrecomp"
"$ROOT/.venv/bin/python" -m tools.recomp "$XBE_PATH" --all --split 250 --game-name "$GAME" \
    --gen-dir "$GAME_ROOT/generated" --disasm-dir "$OUT/disasm" \
    --func-id-dir "$OUT/func_id" --abi-dir "$OUT/abi" \
    --exclude-manual "$GAME_ROOT/src/recomp_manual.c" \
    ${TRACE:+--trace-functions "$TRACE"} $FLAGS
