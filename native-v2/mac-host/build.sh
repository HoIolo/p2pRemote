#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"

swift build -c release

APP_NAME="P2PRemoteMacHost.app"
DIST_DIR="../dist/mac-host"
APP_DIR="$DIST_DIR/$APP_NAME"
CONTENTS="$APP_DIR/Contents"
MACOS="$CONTENTS/MacOS"

rm -rf "$APP_DIR"
mkdir -p "$MACOS"

cp .build/release/p2p-native-mac-host "$MACOS/"
cp Sources/MacHost/Info.plist "$CONTENTS/"

# Ad-hoc code sign so macOS TCC can track screen recording permission by bundle ID
codesign --force --sign - --entitlements /dev/stdin "$APP_DIR" <<'ENTITLEMENTS'
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>com.apple.security.device.audio-input</key>
    <true/>
</dict>
</plist>
ENTITLEMENTS

echo "built: native-v2/dist/mac-host/$APP_NAME (signed ad-hoc)"
