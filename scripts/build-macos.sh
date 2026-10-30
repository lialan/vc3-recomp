#!/usr/bin/env bash
# Build build/VC3.app: the vc3 binary with its Homebrew libraries bundled, and
# a launcher that finds the game folder (vc3.xbe + media/). No game files go
# in the bundle.
#
# Game folder, first that exists: $VC3_GAME_DIR, the folder saved in
# ~/Library/Application Support/VC3/game_dir, ~/Downloads/VR/VC3/extracted,
# else a folder picker (the choice is saved).
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD="${VC3_BUILD_DIR:-$ROOT/build}"
APP="$BUILD/VC3.app"

cmake --build "$BUILD" -j "$(sysctl -n hw.ncpu)"

rm -rf "$APP"
mkdir -p "$APP/Contents/MacOS" "$APP/Contents/Frameworks" "$APP/Contents/Resources"
cp "$BUILD/vc3" "$APP/Contents/MacOS/vc3-bin"

# Copy each non-system library and point its users at the bundled copy.
bundle() {
    local bin=$1 dep base
    for dep in $(otool -L "$bin" | awk 'NR > 1 { print $1 }' | grep '^/opt/homebrew/'); do
        base=$(basename "$dep")
        if [ ! -f "$APP/Contents/Frameworks/$base" ]; then
            cp "$dep" "$APP/Contents/Frameworks/$base"
            chmod u+w "$APP/Contents/Frameworks/$base"
            install_name_tool -id "@rpath/$base" "$APP/Contents/Frameworks/$base"
            bundle "$APP/Contents/Frameworks/$base"
        fi
        install_name_tool -change "$dep" "@rpath/$base" "$bin"
    done
}
bundle "$APP/Contents/MacOS/vc3-bin"
# Drop the build's rpaths (/opt/homebrew/lib), which @rpath would search first.
for rp in $(otool -l "$APP/Contents/MacOS/vc3-bin" | awk '/LC_RPATH/ { getline; getline; print $2 }'); do
    install_name_tool -delete_rpath "$rp" "$APP/Contents/MacOS/vc3-bin"
done
install_name_tool -add_rpath "@executable_path/../Frameworks" "$APP/Contents/MacOS/vc3-bin"

# sdl2-compat loads SDL3 at run time from beside itself.
SDL3="$(brew --prefix sdl3)/lib/libSDL3.0.dylib"
cp "$SDL3" "$APP/Contents/Frameworks/libSDL3.dylib"
chmod u+w "$APP/Contents/Frameworks/libSDL3.dylib"
install_name_tool -id "@loader_path/libSDL3.dylib" "$APP/Contents/Frameworks/libSDL3.dylib"

cat > "$APP/Contents/MacOS/VC3" <<'EOF'
#!/bin/bash
# Find the game folder, then run vc3 with it.
HERE="$(cd "$(dirname "$0")" && pwd)"
CONF="$HOME/Library/Application Support/VC3"
ok() { [ -n "$1" ] && [ -f "$1/vc3.xbe" ]; }
DIR="${VC3_GAME_DIR:-}"
ok "$DIR" || DIR="$(cat "$CONF/game_dir" 2>/dev/null || true)"
ok "$DIR" || DIR="$HOME/Downloads/VR/VC3/extracted"
if ! ok "$DIR"; then
    DIR="$(osascript -e 'POSIX path of (choose folder with prompt "Choose the Virtua Cop 3 folder (vc3.xbe and media/)")' 2>/dev/null || true)"
    DIR="${DIR%/}"
    ok "$DIR" || { osascript -e 'display alert "VC3" message "That folder has no vc3.xbe."' >/dev/null; exit 1; }
fi
mkdir -p "$CONF" && printf '%s' "$DIR" > "$CONF/game_dir"
export VC3_SOUND="${VC3_SOUND:-1}"
# The last session's output, for bug reports (shot positions, errors).
LOGS="$HOME/Library/Logs/VC3"
mkdir -p "$LOGS"
exec "$HERE/vc3-bin" "$DIR" > "$LOGS/vc3.log" 2>&1
EOF
chmod +x "$APP/Contents/MacOS/VC3"

cat > "$APP/Contents/Info.plist" <<'EOF'
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
  <key>CFBundleName</key><string>VC3</string>
  <key>CFBundleDisplayName</key><string>Virtua Cop 3 (recomp)</string>
  <key>CFBundleIdentifier</key><string>org.alanli.vc3-recomp</string>
  <key>CFBundleExecutable</key><string>VC3</string>
  <key>CFBundlePackageType</key><string>APPL</string>
  <key>CFBundleVersion</key><string>0.1</string>
  <key>CFBundleShortVersionString</key><string>0.1</string>
  <key>LSMinimumSystemVersion</key><string>13.0</string>
  <key>NSHighResolutionCapable</key><true/>
</dict>
</plist>
EOF

# install_name_tool breaks the arm64 signatures; re-sign ad hoc.
codesign --force --sign - "$APP/Contents/Frameworks/"*.dylib
codesign --force --sign - "$APP/Contents/MacOS/vc3-bin"
codesign --force --sign - "$APP"
echo "built $APP"
