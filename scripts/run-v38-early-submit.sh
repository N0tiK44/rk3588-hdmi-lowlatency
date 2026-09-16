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
OUTDIR="${OUTDIR:-/tmp/hdmirx-v38}"

if (( EUID != 0 )); then
  echo "ERROR: run with sudo." >&2
  exit 1
fi
if [[ ! -x "$BIN" ]] || ! "$BIN" --help 2>&1 | grep -q -- '--early-submit'; then
  echo "ERROR: V3.8 binary not found; run make clean && make check && make." >&2
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
REF_CSV="$OUTDIR/aligned-reference-b${BUFFERS}.csv"
REF_LOG="$OUTDIR/aligned-reference-b${BUFFERS}.log"
EARLY_CSV="$OUTDIR/early-submit-b${BUFFERS}.csv"
EARLY_LOG="$OUTDIR/early-submit-b${BUFFERS}.log"
REPORT="$OUTDIR/report.txt"
rm -f "$REF_CSV" "$REF_LOG" "$EARLY_CSV" "$EARLY_LOG" "$REPORT"

common=(
  --video "$VIDEO" --card "$CARD"
  --connector "$CONNECTOR" --plane "$PLANE"
  --seconds "$DURATION" --buffers "$BUFFERS"
  --phase-profile --target-refresh-millihz "$TARGET_MILLIHZ"
)

echo "=== RK3588 HDMI-RX / KMS V3.8 early-submission A/B ==="
echo "[1/2] verified ${TARGET_MILLIHZ} mHz aligned reference"
set +e
"$BIN" "${common[@]}" --csv "$REF_CSV" >"$REF_LOG" 2>&1
REF_RC=$?
set -e
if (( REF_RC != 0 )) || [[ ! -s "$REF_CSV" ]]; then
  echo "ERROR: aligned reference failed (exit=$REF_RC). See $REF_LOG" >&2
  tail -n 120 "$REF_LOG" >&2 || true
  exit 1
fi

echo "[2/2] one controlled early-overlap attempt plus phase-prime fallback"
set +e
"$BIN" "${common[@]}" --early-submit --csv "$EARLY_CSV" >"$EARLY_LOG" 2>&1
EARLY_RC=$?
set -e

{
  echo "Kernel: $(uname -r)"
  echo "libdrm: $(pkg-config --modversion libdrm 2>/dev/null || echo unknown)"
  echo "Video: $VIDEO"
  echo "DRM: $CARD connector=$CONNECTOR plane=$PLANE"
  echo "Buffers: $BUFFERS"
  echo "Duration per arm: $DURATION seconds"
  echo "Target: $TARGET_MILLIHZ mHz"
  echo "Early arm exit: $EARLY_RC"
  echo
  if [[ -s "$EARLY_CSV" ]]; then
    python3 "$ROOT/tools/analyze.py" "$REF_CSV" "$EARLY_CSV"
  else
    python3 "$ROOT/tools/analyze.py" "$REF_CSV"
    echo
    echo "Early arm produced no CSV."
  fi
  echo
  echo "V3.8 kernel result counters:"
  grep -E 'V3\.8 (early DQ|OUT before|overlap|other overlap|intentional)' "$EARLY_LOG" || true
  echo
  echo "Interpretation: EBUSY is an expected kernel capability result."
  echo "Only unexpected sequence gaps, missing fences, or FAILED runtime status invalidate the arm."
  echo
  echo "Reference CSV: $REF_CSV"
  echo "Early CSV: $EARLY_CSV"
  echo "Reference log: $REF_LOG"
  echo "Early log: $EARLY_LOG"
} | tee "$REPORT"

if (( EARLY_RC != 0 )); then
  echo "NOTE: early arm exited $EARLY_RC; archive retained for diagnosis." >&2
fi
exit "$EARLY_RC"
