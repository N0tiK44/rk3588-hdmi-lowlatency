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
OUTDIR="${OUTDIR:-/tmp/hdmirx-v37}"

if [[ ${EUID} -ne 0 ]]; then
  echo "ERROR: run with sudo." >&2
  exit 1
fi
if [[ ! -x "$BIN" ]]; then
  echo "ERROR: $BIN not found; run 'make clean && make check && make'." >&2
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
if ! [[ "$TARGET_MILLIHZ" =~ ^[0-9]+$ ]]; then
  echo "ERROR: TARGET_MILLIHZ must be an integer (59940 recommended)." >&2
  exit 2
fi
if ! "$BIN" --help 2>&1 | grep -q -- '--target-refresh-millihz'; then
  echo "ERROR: binary is not the V3.7 cadence-alignment build." >&2
  exit 1
fi

mkdir -p "$OUTDIR"
REF_CSV="$OUTDIR/reference-b${BUFFERS}.csv"
REF_LOG="$OUTDIR/reference-b${BUFFERS}.log"
ALIGNED_CSV="$OUTDIR/aligned-${TARGET_MILLIHZ}mhz-b${BUFFERS}.csv"
ALIGNED_LOG="$OUTDIR/aligned-${TARGET_MILLIHZ}mhz-b${BUFFERS}.log"
REPORT="$OUTDIR/report.txt"

rm -f "$REF_CSV" "$REF_LOG" "$ALIGNED_CSV" "$ALIGNED_LOG" "$REPORT"

common=(
  --video "$VIDEO" --card "$CARD"
  --connector "$CONNECTOR" --plane "$PLANE"
  --seconds "$DURATION" --buffers "$BUFFERS"
  --phase-profile
)

echo "=== RK3588 HDMI-RX / KMS V3.7 cadence-alignment A/B ==="
echo "[1/2] unchanged active output timing"
set +e
"$BIN" "${common[@]}" --csv "$REF_CSV" >"$REF_LOG" 2>&1
REF_RC=$?
set -e

if [[ $REF_RC -ne 0 || ! -s "$REF_CSV" ]]; then
  echo "ERROR: reference run failed (exit=$REF_RC). See $REF_LOG" >&2
  tail -n 100 "$REF_LOG" >&2 || true
  exit 1
fi

echo "[2/2] EDID-advertised ${TARGET_MILLIHZ} mHz output timing"
set +e
"$BIN" "${common[@]}" \
  --target-refresh-millihz "$TARGET_MILLIHZ" \
  --csv "$ALIGNED_CSV" >"$ALIGNED_LOG" 2>&1
ALIGNED_RC=$?
set -e

if [[ $ALIGNED_RC -ne 0 || ! -s "$ALIGNED_CSV" ]]; then
  {
    echo "Kernel: $(uname -r)"
    echo "libdrm: $(pkg-config --modversion libdrm 2>/dev/null || echo unknown)"
    echo
    python3 "$ROOT/tools/analyze.py" "$REF_CSV"
    echo
    echo "V3.7 aligned run did not complete (exit=$ALIGNED_RC)."
    echo "The monitor may not advertise an exact 59.94 Hz mode, or the driver may reject the atomic modeset."
    echo "No synthetic timing or fallback was attempted. See: $ALIGNED_LOG"
    echo
    tail -n 120 "$ALIGNED_LOG" || true
  } | tee "$REPORT"
  exit 1
fi

if ! grep -q 'V3.7 target timing verified active' "$ALIGNED_LOG"; then
  echo "ERROR: aligned run produced data without confirming the target timing." >&2
  exit 1
fi

{
  echo "Kernel: $(uname -r)"
  echo "libdrm: $(pkg-config --modversion libdrm 2>/dev/null || echo unknown)"
  echo "Video: $VIDEO"
  echo "DRM: $CARD connector=$CONNECTOR plane=$PLANE"
  echo "Buffers: $BUFFERS"
  echo "Duration per arm: $DURATION seconds"
  echo "Target: $TARGET_MILLIHZ mHz"
  echo
  python3 "$ROOT/tools/analyze.py" "$REF_CSV" "$ALIGNED_CSV"
  echo
  echo "Reference CSV: $REF_CSV"
  echo "Aligned CSV: $ALIGNED_CSV"
  echo "Reference log: $REF_LOG"
  echo "Aligned log: $ALIGNED_LOG"
} | tee "$REPORT"

echo "Report: $REPORT"
