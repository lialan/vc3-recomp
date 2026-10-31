#!/usr/bin/env bash
# Install a Chihiro game's Quest app on a headset over adb, and copy the game
# to it (the names from games/<id>/game.conf; default vc3, Virtua Cop 3 VR).
#
#   quest/install.sh [id]                      the APK only (an update)
#   quest/install.sh [id] /path/to/the/game    the APK and the game's files
#
# The game folder is the one the desktop build runs from (the XBE, media/
# and the rest beside them); its contents go to the game's HEADSET_DIR
# (VC3: /sdcard/VirtuaCop3, about 460 MB), once. With more than one device
# attached, pick one with ANDROID_SERIAL=<serial>.
set -e
Q=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$Q/.." && pwd)
. "$ROOT/scripts/game.sh" "$@"
set -- ${GAME_ARGS[@]+"${GAME_ARGS[@]}"}
DEST=$HEADSET_DIR
adb install -r "$Q/$APK.apk"
# the game is read from shared storage
adb shell appops set --uid "$PKG" MANAGE_EXTERNAL_STORAGE allow
if [ -n "${1:-}" ]; then
    [ -f "$1/$XBE" ] && [ -d "$1/media" ] || { echo "$1 has no $XBE and media/"; exit 1; }
    echo "copying the game to $DEST"
    adb shell mkdir -p "$DEST"
    adb push "$1/." "$DEST/"            # all of it: the firmware images too
fi
echo "Installed. Start \"$APP\" from Unknown Sources."
echo "Log of the last run: adb shell cat $DEST/$LOG"
