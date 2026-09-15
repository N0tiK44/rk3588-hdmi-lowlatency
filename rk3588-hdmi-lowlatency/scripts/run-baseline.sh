#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BIN="${BIN:-$ROOT/hdmirx-kms-lowlat}"
VIDEO="${VIDEO:-/dev/video0}"
CARD="${CARD:-/dev/dri/card0}"
CONNECTOR="${CONNECTOR:-217}"
PLANE="${PLANE:-114}"
DURATION="${DURATION:-30}"
BUFFERS="${BUFFERS:-4}"
OUTDIR="${OUTDIR:-/tmp/hdmirx-baseline}"

if [[ ${EUID} -ne 0 ]]; then
  echo "ERROR: run with sudo (DRM master/device access and low_latency control are required)." >&2
  exit 1
fi
if [[ ! -x "$BIN" ]]; then
  echo "ERROR: $BIN not found; run 'make' first." >&2
  exit 1
fi

mkdir -p "$OUTDIR"
CSV="$OUTDIR/baseline-b${BUFFERS}.csv"
LOG="$OUTDIR/baseline-b${BUFFERS}.log"
REPORT="$OUTDIR/report.txt"
rm -f "$CSV" "$LOG" "$REPORT"

echo "=== RK3588 HDMI-RX baseline ==="
echo "video=$VIDEO card=$CARD connector=$CONNECTOR plane=$PLANE buffers=$BUFFERS duration=${DURATION}s"

set +e
"$BIN" \
  --video "$VIDEO" --card "$CARD" \
  --connector "$CONNECTOR" --plane "$PLANE" \
  --buffers "$BUFFERS" --seconds "$DURATION" \
  --csv "$CSV" >"$LOG" 2>&1
RC=$?
set -e

if [[ -s "$CSV" ]]; then
  python3 "$ROOT/tools/analyze.py" "$CSV" | tee "$REPORT"
else
  echo "No CSV was produced. Tail of log:" >&2
  tail -n 100 "$LOG" >&2 || true
fi

if [[ $RC -ne 0 ]]; then
  echo "Baseline exited with status $RC. Full log: $LOG" >&2
  exit "$RC"
fi

echo "CSV:    $CSV"
echo "Log:    $LOG"
echo "Report: $REPORT"
