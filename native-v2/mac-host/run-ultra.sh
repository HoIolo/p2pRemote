#!/usr/bin/env bash
set -euo pipefail
CLIENT_IP="${CLIENT_IP:-192.168.1.50}"
WIDTH="${WIDTH:-1920}"
HEIGHT="${HEIGHT:-1080}"
FPS="${FPS:-60}"
BITRATE="${BITRATE:-30000000}"
VIDEO_PORT="${VIDEO_PORT:-45000}"
INPUT_PORT="${INPUT_PORT:-45001}"
TRANSPORT="${TRANSPORT:-udp}"
CAPTURE_MODE="${CAPTURE_MODE:-fill}"
SHOW_HOST_CURSOR="${SHOW_HOST_CURSOR:-0}"

EXTRA_ARGS=()
if [[ "$SHOW_HOST_CURSOR" == "1" || "$SHOW_HOST_CURSOR" == "true" ]]; then
  EXTRA_ARGS+=(--show-host-cursor)
fi
cd "$(dirname "$0")"

APP_DIR="../dist/mac-host/P2PRemoteMacHost.app"
BIN="$APP_DIR/Contents/MacOS/p2p-native-mac-host"

if [[ ! -x "$BIN" ]]; then
  bash build.sh
fi

exec "$BIN" \
  --client-ip "$CLIENT_IP" \
  --video-port "$VIDEO_PORT" \
  --input-port "$INPUT_PORT" \
  --width "$WIDTH" \
  --height "$HEIGHT" \
  --fps "$FPS" \
  --bitrate "$BITRATE" \
  --keyint 2 \
  --transport "$TRANSPORT" \
  --capture-mode "$CAPTURE_MODE" \
  "${EXTRA_ARGS[@]}"
