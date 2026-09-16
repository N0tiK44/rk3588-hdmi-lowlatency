#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BIN="${BIN:-$ROOT/hdmirx-kms-lowlat}"
VIDEO="${VIDEO:-/dev/video0}"
CARD="${CARD:-/dev/dri/card0}"
CONNECTOR="${CONNECTOR:-217}"
PLANE="${PLANE:-114}"
DURATION="${DURATION:-120}"
BUFFERS="${BUFFERS:-4}"
TARGET_MILLIHZ="${TARGET_MILLIHZ:-59940}"
OUTDIR="${OUTDIR:-/tmp/hdmirx-v381}"

if (( EUID != 0 )); then
  echo "ERROR: run with sudo." >&2
  exit 1
fi
if [[ ! -x "$BIN" ]] || ! "$BIN" --help 2>&1 | grep -q 'V3.8.1 safe'; then
  echo "ERROR: V3.8.1 binary not found; run make clean && make check && make." >&2
  exit 1
fi
if ! [[ "$BUFFERS" =~ ^[0-9]+$ ]] || (( BUFFERS < 4 || BUFFERS > 8 )); then
  echo "ERROR: BUFFERS must be 4..8; four is the proven minimum." >&2
  exit 2
fi
if ! [[ "$DURATION" =~ ^[0-9]+$ ]] || (( DURATION < 30 )); then
  echo "ERROR: DURATION must be at least 30 seconds (120 recommended)." >&2
  exit 2
fi

mkdir -p "$OUTDIR"
CSV="$OUTDIR/safe-window-b${BUFFERS}.csv"
LOG="$OUTDIR/safe-window-b${BUFFERS}.log"
REPORT="$OUTDIR/report.txt"
rm -f "$CSV" "$LOG" "$REPORT"

echo "=== RK3588 HDMI-RX / KMS V3.8.1 safe early-window profiler ==="
echo "One uninterrupted arm; no overlapping commit and no intentional drop."

set +e
"$BIN" \
  --video "$VIDEO" --card "$CARD" \
  --connector "$CONNECTOR" --plane "$PLANE" \
  --seconds "$DURATION" --buffers "$BUFFERS" \
  --phase-profile --target-refresh-millihz "$TARGET_MILLIHZ" \
  --early-submit --csv "$CSV" >"$LOG" 2>&1
RUN_RC=$?
set -e

{
  echo "Kernel: $(uname -r)"
  echo "libdrm: $(pkg-config --modversion libdrm 2>/dev/null || echo unknown)"
  echo "Video: $VIDEO"
  echo "DRM: $CARD connector=$CONNECTOR plane=$PLANE"
  echo "Buffers: $BUFFERS"
  echo "Duration: $DURATION seconds"
  echo "Target: $TARGET_MILLIHZ mHz"
  echo "Run exit: $RUN_RC"
  echo
  if [[ -s "$CSV" ]]; then
    python3 "$ROOT/tools/analyze.py" "$CSV"
  else
    echo "No CSV was produced."
  fi
  echo
  echo "V3.8.1 window counters:"
  grep -E 'V3\.8\.1 (capture|OUT|genuine|post-latch|intentional|probe)' "$LOG" || true
  echo
  echo "Pass requirements: runtime completed, missing fences 0, sequence gaps 0, intentional drops 0."
  echo "CSV: $CSV"
  echo "Log: $LOG"
} | tee "$REPORT"

if (( RUN_RC != 0 )); then
  echo "NOTE: profiler exited $RUN_RC; archive retained for diagnosis." >&2
fi
exit "$RUN_RC"
