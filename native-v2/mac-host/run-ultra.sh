#!/usr/bin/env bash
set -euo pipefail
CLIENT_IP="${CLIENT_IP:-192.168.1.50}"
WIDTH="${WIDTH:-1920}"
HEIGHT="${HEIGHT:-1080}"
FPS="${FPS:-60}"
BITRATE="${BITRATE:-18000000}"
VIDEO_PORT="${VIDEO_PORT:-45000}"
INPUT_PORT="${INPUT_PORT:-45001}"
TRANSPORT="${TRANSPORT:-udp}"
cd "$(dirname "$0")"
BIN=".build/release/p2p-native-mac-host"
if [[ ! -x "$BIN" ]]; then
  swift build -c release
fi
exec "$BIN" \
  --client-ip "$CLIENT_IP" \
  --video-port "$VIDEO_PORT" \
  --input-port "$INPUT_PORT" \
  --width "$WIDTH" \
  --height "$HEIGHT" \
  --fps "$FPS" \
  --bitrate "$BITRATE" \
  --keyint 6 \
  --transport "$TRANSPORT"
