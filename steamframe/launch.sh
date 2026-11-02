#!/bin/sh
# Virtua Cop 3 (recomp) VR on Steam Frame: native AArch64 Linux, Vulkan,
# OpenXR through SteamVR -- the Quest version's VR (screen in front of you,
# a handgun in your hand, its settings panel). launch-flat.sh: the flat game.
#
# The game folder (vc3.xbe + media/) is ~/work/vc3-game unless game-dir.txt
# beside this script names another. Options: env.txt beside this script
# (NAME=value per line). Log of the last run: vc3.log beside this script.
HERE=$(cd "$(dirname "$0")" && pwd)
GAME_DIR=$HOME/work/vc3-game
[ -f "$HERE/game-dir.txt" ] && GAME_DIR=$(cat "$HERE/game-dir.txt")
[ -f "$GAME_DIR/vc3.xbe" ] || { echo "no vc3.xbe in $GAME_DIR" >&2; exit 1; }

# the OpenXR loader (and its jsoncpp) beside the game: SteamOS has none
export LD_LIBRARY_PATH="$HERE/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
export VC3_SOUND=1
export SDL_AUDIODRIVER="${SDL_AUDIODRIVER:-pipewire}"
export VC3_PACE_LOG="${VC3_PACE_LOG:-1}"        # frame pacing lines in vc3.log
export VC3_SAVE_DIR="${VC3_SAVE_DIR:-$HERE/save}" VC3_CONFIG_DIR="${VC3_CONFIG_DIR:-$HERE/config}"
mkdir -p "$VC3_SAVE_DIR" "$VC3_CONFIG_DIR"
if [ -f "$HERE/env.txt" ]; then
    while IFS= read -r line || [ -n "$line" ]; do
        case $line in ''|'#'*) continue ;; *=*) export "$line" ;; esac
    done < "$HERE/env.txt"
fi
ulimit -s 262144 2>/dev/null || ulimit -s unlimited 2>/dev/null || true

# SteamVR on the Frame runs every app at its per-app refresh rate (72 Hz
# unless set) whatever the game asks for: set this game's to the panel's
# choice (vr_refresh in the settings, 120 by default), for the key SteamVR
# files it under when Steam starts it and when it is started by hand.
HZ=$(sed -n 's/^vr_refresh=//p' "$VC3_CONFIG_DIR/settings" 2>/dev/null)
VRCMD=/opt/steamvr/bin/linuxarm64/vrcmd
if [ -x "$VRCMD" ]; then
    for key in ${SteamAppId:+"steam.app.$SteamAppId"} ${SteamGameId:+"steam.app.$SteamGameId"} \
               "system.generated.openxr.virtua cop 3 vr.vc3"; do
        LD_LIBRARY_PATH=/opt/steamvr/bin/linuxarm64 timeout 5 "$VRCMD" \
            --set-settings-float "$key.preferredRefreshRate" "${HZ:-120}" >/dev/null 2>&1
    done
fi
exec "$HERE/vc3" "$GAME_DIR" "$@" > "$HERE/vc3.log" 2>&1
