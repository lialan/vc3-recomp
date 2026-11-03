#!/bin/sh
# Virtua Cop 3 (recomp) on Steam Frame: native AArch64 Linux, Vulkan renderer,
# flat (no VR yet) -- the game in a fullscreen window on Steam's screen.
#
#   ./launch.sh            # add it to Steam as a non-Steam game to start it
#
# The game folder (vc3.xbe + media/) is ~/work/vc3-game unless game-dir.txt
# beside this script names another. Options: env.txt beside this script
# (NAME=value per line, e.g. VC3_SCALE=2, VC3_ASPECT=16:9, VC3_FPS=120).
# Log of the last run: vc3.log beside this script.
#
# Controls, with Steam's default gamepad layout:
#   either stick aims (a red crosshair shows where), right trigger or A fires,
#   left trigger or B reloads, bumpers or Y = pedal (E.S.), X = CHANGE,
#   Start = coin + start, Back = coin.
# Mouse/keyboard also work: mouse aims, left fires, right reloads, middle =
#   CHANGE, Space = pedal, 5 = coin, 1 = start, F2 test, F10 60/120 fps.
HERE=$(cd "$(dirname "$0")" && pwd)
GAME_DIR=$HOME/work/vc3-game
[ -f "$HERE/game-dir.txt" ] && GAME_DIR=$(cat "$HERE/game-dir.txt")
[ -f "$GAME_DIR/vc3.xbe" ] || { echo "no vc3.xbe in $GAME_DIR" >&2; exit 1; }

export VC3_SOUND=1 RECOMP_FULLSCREEN=1
# SteamOS: SDL's PulseAudio backend never finishes opening (the game sat on
# a black screen); its native PipeWire backend works.
export SDL_AUDIODRIVER="${SDL_AUDIODRIVER:-pipewire}"
# Steam's virtual gamepad is the controllers; SDL skips it unless told.
export SDL_GAMECONTROLLER_ALLOW_STEAM_VIRTUAL_GAMEPAD=1
export VC3_SAVE_DIR="${VC3_SAVE_DIR:-$HERE/save}" VC3_CONFIG_DIR="${VC3_CONFIG_DIR:-$HERE/config}"
mkdir -p "$VC3_SAVE_DIR" "$VC3_CONFIG_DIR"
# Pipeline recipes recorded on the Frame (pipelines.recipes beside the
# game): added to the save's once per seed, so the game precompiles them
# while it boots instead of compiling mid-play.
if [ -f "$HERE/pipelines.recipes" ]; then
    SEED=$(cksum < "$HERE/pipelines.recipes" | cut -d' ' -f1)
    if [ ! -f "$VC3_SAVE_DIR/.recipes-seed-$SEED" ]; then
        cat "$HERE/pipelines.recipes" >> "$VC3_SAVE_DIR/vulkan-pipelines.recipes" &&
            touch "$VC3_SAVE_DIR/.recipes-seed-$SEED"
    fi
fi
if [ -f "$HERE/env.txt" ]; then
    while IFS= read -r line || [ -n "$line" ]; do
        case $line in ''|'#'*) continue ;; *=*) export "$line" ;; esac
    done < "$HERE/env.txt"
fi
ulimit -s 262144 2>/dev/null || ulimit -s unlimited 2>/dev/null || true
exec "$HERE/vc3-flat" "$GAME_DIR" "$@" > "$HERE/vc3.log" 2>&1
