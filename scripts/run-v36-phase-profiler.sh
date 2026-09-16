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
OUTDIR="${OUTDIR:-/tmp/hdmirx-v36}"

if [[ ${EUID} -ne 0 ]]; then
  echo "ERROR: run with sudo." >&2
  exit 1
fi
if [[ ! -x "$BIN" ]]; then
  echo "ERROR: $BIN not found; run 'make clean && make check && make' first." >&2
  exit 1
fi
if ! [[ "$BUFFERS" =~ ^[0-9]+$ ]] || (( BUFFERS < 4 || BUFFERS > 8 )); then
  echo "ERROR: BUFFERS must be 4..8; four is the proven minimum." >&2
  exit 2
fi
if ! [[ "$DURATION" =~ ^[0-9]+$ ]] || (( DURATION < 20 )); then
  echo "ERROR: DURATION must be at least 20 seconds (120 recommended)." >&2
  exit 2
fi
if ! "$BIN" --help 2>&1 | grep -q -- '--phase-profile'; then
  echo "ERROR: binary is not the V3.6 phase-profiler build." >&2
  exit 1
fi
for node in "$VIDEO" "$CARD"; do
  [[ -e "$node" ]] || { echo "ERROR: missing device $node" >&2; exit 1; }
done

mkdir -p "$OUTDIR"
CSV="$OUTDIR/phase-b${BUFFERS}.csv"
LOG="$OUTDIR/phase-b${BUFFERS}.log"
REPORT="$OUTDIR/report.txt"
rm -f "$CSV" "$LOG" "$REPORT"

echo "=== RK3588 HDMI-RX / KMS V3.6 phase profiler ==="
echo "Normal atomic path; no async flag and no datapath change."
echo "duration=${DURATION}s buffers=$BUFFERS"

set +e
"$BIN" \
  --video "$VIDEO" --card "$CARD" \
  --connector "$CONNECTOR" --plane "$PLANE" \
  --seconds "$DURATION" --buffers "$BUFFERS" \
  --phase-profile --csv "$CSV" >"$LOG" 2>&1
RC=$?
set -e

if [[ $RC -ne 0 || ! -s "$CSV" ]]; then
  echo "ERROR: V3.6 profiler failed (exit=$RC). See $LOG" >&2
  tail -n 100 "$LOG" >&2 || true
  [[ $RC -ne 0 ]] && exit "$RC"
  exit 1
fi

{
  echo "Kernel: $(uname -r)"
  echo "libdrm: $(pkg-config --modversion libdrm 2>/dev/null || echo unknown)"
  echo "Video: $VIDEO"
  echo "DRM: $CARD connector=$CONNECTOR plane=$PLANE"
  echo "Buffers: $BUFFERS"
  echo "Duration: $DURATION seconds"
  echo
  python3 "$ROOT/tools/analyze.py" "$CSV"
  echo
  echo "CSV: $CSV"
  echo "Log: $LOG"
} | tee "$REPORT"

echo "Report: $REPORT"
