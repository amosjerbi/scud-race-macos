#!/bin/bash
# Assemble native-mac/ScudRace.app (it finds the game files in its own folder or the parent folder, next to scudrace.exe / scud.zip / ui/).
set -euo pipefail
cd "$(dirname "$0")"
./build.sh >/dev/null
APP=${1:-$PWD/ScudRace.app}
rm -rf "$APP"; mkdir -p "$APP/Contents/MacOS" "$APP/Contents/Frameworks" "$APP/Contents/Resources"
cp build/scudrace-host "$APP/Contents/MacOS/ScudRace"
cp build/Frameworks/libSDL2-2.0.0.dylib "$APP/Contents/Frameworks/"
cat > "$APP/Contents/Info.plist" <<'P'
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict>
 <key>CFBundleName</key><string>Scud Race</string>
 <key>CFBundleDisplayName</key><string>Scud Race</string>
 <key>CFBundleIdentifier</key><string>local.scudrace.macos</string>
 <key>CFBundleExecutable</key><string>ScudRace</string>
 <key>CFBundlePackageType</key><string>APPL</string>
 <key>CFBundleShortVersionString</key><string>0.0.14</string>
 <key>CFBundleVersion</key><string>0.0.14</string>
 <key>LSMinimumSystemVersion</key><string>11.0</string>
 <key>NSHighResolutionCapable</key><true/>
 <key>LSApplicationCategoryType</key><string>public.app-category.games</string>
 <key>NSSupportsAutomaticGraphicsSwitching</key><true/>
</dict></plist>
P
codesign --force --deep -s - "$APP" 2>/dev/null
echo "built $APP"
