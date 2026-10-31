#!/usr/bin/env bash
# quest/es-record.sh <apk> <scenario> <out-dir>: one devtest.sh scenario
# (an E.S. one: the pedal held 64-75 s), with the headset's screen recorded
# for 4 s from the game's "auto pedal at $AT" line (66 s by default) (screenrecord), pulled,
# and cut into BMP frames (ffmpeg) in <out-dir> for checking frame to frame
# -- the E.S. flicker. screenrecord takes the compositor's view (both
# eyes, through the lenses' distortion) at 5-10 frames a second, not every
# frame; a flicker shows as the scene coming and going between them. The
# recording is deleted from the headset.
set -u
SERIAL=${SERIAL:-2G97C5ZJ0G02DQ}
SDK=${ANDROID_SDK_ROOT:-$HOME/Library/Android/sdk}
A="$SDK/platform-tools/adb -s $SERIAL"
ROOT=$(cd "$(dirname "$0")/.." && pwd)
APK=$1; SCEN=$2; OUT=$3
mkdir -p "$OUT"
bash "$ROOT/quest/devtest.sh" --game vc3 --apk "$APK" --only "$SCEN" > "$OUT/devtest.out" 2>&1 &
dt=$!
for i in $(seq 1 120); do                   # devtest.sh clears the log just after this line
    grep -q "^== $SCEN" "$OUT/devtest.out" 2>/dev/null && break
    sleep 1
done
sleep 5
for i in $(seq 1 200); do                   # the pedal held two seconds: E.S. on
    $A logcat -d -s vc3:I 2>/dev/null | grep -q "auto pedal at ${AT:-66}" && break
    sleep 0.5
done
$A shell screenrecord --time-limit 4 /sdcard/es_rec.mp4
$A pull /sdcard/es_rec.mp4 "$OUT/es_rec.mp4" >/dev/null 2>&1
$A shell rm -f /sdcard/es_rec.mp4
wait $dt
ffmpeg -loglevel error -i "$OUT/es_rec.mp4" -vf "scale=1032:-2" -fps_mode passthrough "$OUT/f%03d.bmp" 2>/dev/null
echo "frames: $(ls "$OUT"/f*.bmp 2>/dev/null | wc -l)"
grep -v pushed "$OUT/devtest.out" | grep -v '^   \[' | tail -3
