#!/usr/bin/env bash
# Short, muted bring-up run: scripts/run-test.sh [seconds] [log]
# Game dir from $VC3_GAME_DIR (default ~/Downloads/VR/VC3/extracted); saves go
# to a scratch dir unless VC3_SAVE_DIR is set. Extra RECOMP_* vars pass through.
set -uo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SECS="${1:-45}"
LOG="${2:-$ROOT/run.log}"
GAME="${VC3_GAME_DIR:-$HOME/Downloads/VR/VC3/extracted}"
export VC3_SAVE_DIR="${VC3_SAVE_DIR:-$ROOT/build/save}"
export RECOMP_WATCHDOG_SECS="${RECOMP_WATCHDOG_SECS:-$((SECS - 5))}"
unset VC3_SOUND
mkdir -p "$VC3_SAVE_DIR"
timeout -k 5 "$SECS" "$ROOT/build/vc3" "$GAME" >"$LOG" 2>&1
rc=$?
echo "exit=$rc lines=$(wc -l <"$LOG") log=$LOG"
