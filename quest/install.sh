#!/bin/sh
# Install Virtua Cop 3 VR on a headset over adb, and copy the game to it.
#
#   quest/install.sh                        the APK only (an update)
#   quest/install.sh /path/to/vc3           the APK and the game's files
#
# The game folder is the one the desktop build runs from (vc3.xbe, media/ and
# the rest beside them); its contents go to /sdcard/VirtuaCop3 (about
# 460 MB), once. With more than one
# device attached, pick one with ANDROID_SERIAL=<serial>.
set -e
Q=$(cd "$(dirname "$0")" && pwd)
PKG=org.vc3recomp.vr
DEST=/sdcard/VirtuaCop3
adb install -r "$Q/VirtuaCop3VR.apk"
# the game is read from shared storage
adb shell appops set --uid "$PKG" MANAGE_EXTERNAL_STORAGE allow
if [ -n "$1" ]; then
    [ -f "$1/vc3.xbe" ] && [ -d "$1/media" ] || { echo "$1 has no vc3.xbe and media/"; exit 1; }
    echo "copying the game to $DEST"
    adb shell mkdir -p "$DEST"
    adb push "$1/." "$DEST/"            # all of it: the firmware images too
fi
echo "Installed. Start \"Virtua Cop 3 VR\" from Unknown Sources."
echo "Log of the last run: adb shell cat $DEST/vc3.log"
