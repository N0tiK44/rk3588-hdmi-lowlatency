#!/usr/bin/env bash
set -euo pipefail

ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
BIN="$ROOT/rk3588-hdmi-passthrough"
OUTDIR=${OUTDIR:-/tmp/hdmirx-v1}
DURATION=${DURATION:-120}
BUFFERS=${BUFFERS:-4}
VIDEO=${VIDEO:-/dev/video0}
CARD=${CARD:-/dev/dri/card0}
CONNECTOR=${CONNECTOR:-217}
PLANE=${PLANE:-114}
TARGET_REFRESH_MILLIHZ=${TARGET_REFRESH_MILLIHZ:-59940}

if (( EUID != 0 )); then
  echo "Run this script with sudo." >&2
  exit 2
fi

if [[ ! -x "$BIN" ]]; then
  echo "Binary missing. Run: make clean && make check && make" >&2
  exit 2
fi

install -d -m 0755 "$OUTDIR"
CSV="$OUTDIR/trace.csv"
LOG="$OUTDIR/run.log"
REPORT="$OUTDIR/report.txt"
META="$OUTDIR/metadata.txt"

{
  echo "version=1.0"
  echo "timestamp_utc=$(date -u +%Y-%m-%dT%H:%M:%SZ)"
  echo "commit=$(git -C "$ROOT" rev-parse HEAD 2>/dev/null || echo unknown)"
  echo "kernel=$(uname -r)"
  echo "architecture=$(uname -m)"
  echo "libdrm=$(pkg-config --modversion libdrm 2>/dev/null || echo unknown)"
  echo "duration_seconds=$DURATION"
  echo "buffers=$BUFFERS"
  echo "video=$VIDEO"
  echo "card=$CARD"
  echo "connector=$CONNECTOR"
  echo "plane=$PLANE"
  echo "target_refresh_millihz=$TARGET_REFRESH_MILLIHZ"
} >"$META"

echo "=== RK3588 HDMI passthrough V1.0 diagnostic ==="
echo "One ${DURATION}-second arm; no frame drops or overlapping commits."

set +e
"$BIN" \
  --video "$VIDEO" \
  --card "$CARD" \
  --connector "$CONNECTOR" \
  --plane "$PLANE" \
  --seconds "$DURATION" \
  --buffers "$BUFFERS" \
  --target-refresh-millihz "$TARGET_REFRESH_MILLIHZ" \
  --phase-profile \
  --window-profile \
  --csv "$CSV" 2>&1 | tee "$LOG"
RUN_RC=${PIPESTATUS[0]}
set -e

echo "run_exit=$RUN_RC" >>"$META"

if [[ -s "$CSV" ]]; then
  python3 "$ROOT/tools/analyze.py" "$CSV" | tee "$REPORT"
else
  echo "No timing CSV was produced; inspect $LOG" | tee "$REPORT"
fi

echo
echo "Results: $OUTDIR"
exit "$RUN_RC"
