#!/usr/bin/env bash
set -euo pipefail

ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
BIN="$ROOT/rk3588-hdmi-passthrough"
DURATION=${DURATION:-0}
BUFFERS=${BUFFERS:-4}
VIDEO=${VIDEO:-/dev/video0}
CARD=${CARD:-/dev/dri/card0}
CONNECTOR=${CONNECTOR:-217}
PLANE=${PLANE:-114}
TARGET_REFRESH_MILLIHZ=${TARGET_REFRESH_MILLIHZ:-59940}
CSV=${CSV:-/tmp/hdmirx-live.csv}

if (( EUID != 0 )); then
  echo "Run this script with sudo." >&2
  exit 2
fi

if [[ ! -x "$BIN" ]]; then
  echo "Binary missing. Run: make clean && make check && make" >&2
  exit 2
fi

if [[ "$DURATION" == 0 ]]; then
  DURATION=2147483647
fi

exec "$BIN" \
  --video "$VIDEO" \
  --card "$CARD" \
  --connector "$CONNECTOR" \
  --plane "$PLANE" \
  --seconds "$DURATION" \
  --buffers "$BUFFERS" \
  --target-refresh-millihz "$TARGET_REFRESH_MILLIHZ" \
  --csv "$CSV"
