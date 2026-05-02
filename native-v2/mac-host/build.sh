#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"
swift build -c release
mkdir -p ../dist/mac-host
cp .build/release/p2p-native-mac-host ../dist/mac-host/
echo "built: native-v2/dist/mac-host/p2p-native-mac-host"
