#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BIN="${BIN:-$ROOT/hdmirx-kms-lowlat}"
VIDEO="${VIDEO:-/dev/video0}"
CARD="${CARD:-/dev/dri/card0}"
CONNECTOR="${CONNECTOR:-217}"
PLANE="${PLANE:-114}"
DURATION="${DURATION:-20}"
BUFFERS="${BUFFERS:-4}"
OUTDIR="${OUTDIR:-/tmp/hdmirx-v35}"

if [[ ${EUID} -ne 0 ]]; then
  echo "ERROR: run with sudo." >&2
  exit 1
fi
if [[ ! -x "$BIN" ]]; then
  echo "ERROR: $BIN not found; run 'make' first." >&2
  exit 1
fi
if ! "$BIN" --help 2>&1 | grep -q -- '--async-flip'; then
  echo "ERROR: binary does not contain the consolidated V3.5 async test." >&2
  exit 1
fi

mkdir -p "$OUTDIR"
SYNC="$OUTDIR/sync-b${BUFFERS}.csv"
ASYNC="$OUTDIR/async-b${BUFFERS}.csv"
SLOG="$OUTDIR/sync-b${BUFFERS}.log"
ALOG="$OUTDIR/async-b${BUFFERS}.log"
REPORT="$OUTDIR/report.txt"
rm -f "$SYNC" "$ASYNC" "$SLOG" "$ALOG" "$REPORT"

common=(
  --video "$VIDEO" --card "$CARD"
  --connector "$CONNECTOR" --plane "$PLANE"
  --seconds "$DURATION" --buffers "$BUFFERS"
)

echo "=== HDMI-RX / KMS V3.5 phase-bypass A/B test ==="
echo "[1/2] normal atomic path"
"$BIN" "${common[@]}" --csv "$SYNC" >"$SLOG" 2>&1

echo "[2/2] DRM_MODE_PAGE_FLIP_ASYNC path (experimental)"
set +e
"$BIN" "${common[@]}" --async-flip --csv "$ASYNC" >"$ALOG" 2>&1
ARC=$?
set -e

if [[ $ARC -ne 0 || ! -s "$ASYNC" ]]; then
  {
    python3 "$ROOT/tools/analyze.py" "$SYNC"
    echo
    echo "ASYNC run did not complete successfully (exit=$ARC)."
    echo "This usually means the driver rejected async atomic flips or the path failed before producing samples."
    echo "See: $ALOG"
  } | tee "$REPORT"
  tail -n 80 "$ALOG" >&2 || true
  exit 0
fi

python3 "$ROOT/tools/analyze.py" "$SYNC" "$ASYNC" | tee "$REPORT"
echo "Report: $REPORT"
