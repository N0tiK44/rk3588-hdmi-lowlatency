#!/usr/bin/env bash
set -u -o pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
EXPERIMENT="${1:-v37}"
RESULTS_BASE="${RESULTS_BASE:-$HOME/hdmirx-results}"
BUFFERS="${BUFFERS:-4}"
VIDEO="${VIDEO:-/dev/video0}"
CARD="${CARD:-/dev/dri/card0}"
CONNECTOR="${CONNECTOR:-217}"
PLANE="${PLANE:-114}"
TARGET_MILLIHZ="${TARGET_MILLIHZ:-59940}"

case "$EXPERIMENT" in
  baseline)
    RUNNER="$ROOT/scripts/run-baseline.sh"
    DEFAULT_DURATION=30
    ;;
  v35)
    RUNNER="$ROOT/scripts/run-v35-phase.sh"
    DEFAULT_DURATION=20
    ;;
  v36)
    RUNNER="$ROOT/scripts/run-v36-phase-profiler.sh"
    DEFAULT_DURATION=120
    ;;
  v37)
    RUNNER="$ROOT/scripts/run-v37-cadence-alignment.sh"
    DEFAULT_DURATION=120
    ;;
  *)
    echo "ERROR: unknown experiment '$EXPERIMENT'." >&2
    exit 2
    ;;
esac

DURATION="${DURATION:-$DEFAULT_DURATION}"
STAMP="$(date -u +%Y%m%dT%H%M%SZ)"
COMMIT="$(git -C "$ROOT" rev-parse --short=12 HEAD)"
RUN_ID="hdmirx-${EXPERIMENT}-${STAMP}-${COMMIT}"
RUN_DIR="$RESULTS_BASE/$RUN_ID"
ARCHIVE="$RESULTS_BASE/$RUN_ID.tar.gz"
LATEST="$RESULTS_BASE/latest.tar.gz"

mkdir -p "$RUN_DIR"

echo "=== Validating and building $COMMIT ==="
set +e
make -C "$ROOT" clean
CLEAN_RC=$?
make -C "$ROOT" check
CHECK_RC=$?
make -C "$ROOT"
BUILD_RC=$?
set -e

if (( CLEAN_RC != 0 || CHECK_RC != 0 || BUILD_RC != 0 )); then
  RUN_RC=125
  echo "ERROR: build validation failed; the hardware test was not started." >&2
else
  echo "=== Running $EXPERIMENT for $DURATION seconds per arm ==="
  echo "Results directory: $RUN_DIR"
  sudo -v
  set +e
  sudo env \
    OUTDIR="$RUN_DIR" \
    DURATION="$DURATION" \
    BUFFERS="$BUFFERS" \
    VIDEO="$VIDEO" \
    CARD="$CARD" \
    CONNECTOR="$CONNECTOR" \
    PLANE="$PLANE" \
    TARGET_MILLIHZ="$TARGET_MILLIHZ" \
    bash "$RUNNER" 2>&1 | tee "$RUN_DIR/console.txt"
  RUN_RC=${PIPESTATUS[0]}
  set -e
fi

# Device runners execute as root. Return every result to the login user before
# packaging so SCP never needs root access.
sudo chown -R "$(id -u):$(id -g)" "$RUN_DIR" 2>/dev/null || true

{
  echo "run_id=$RUN_ID"
  echo "experiment=$EXPERIMENT"
  echo "started_archive_utc=$STAMP"
  echo "git_commit=$(git -C "$ROOT" rev-parse HEAD)"
  echo "git_describe=$(git -C "$ROOT" describe --always --dirty 2>/dev/null || true)"
  echo "runner_exit=$RUN_RC"
  echo "build_clean_exit=$CLEAN_RC"
  echo "build_check_exit=$CHECK_RC"
  echo "build_exit=$BUILD_RC"
  echo "kernel=$(uname -r)"
  echo "machine=$(uname -m)"
  echo "libdrm=$(pkg-config --modversion libdrm 2>/dev/null || echo unknown)"
  echo "video=$VIDEO"
  echo "card=$CARD"
  echo "connector=$CONNECTOR"
  echo "plane=$PLANE"
  echo "buffers=$BUFFERS"
  echo "duration_per_arm=$DURATION"
  echo "target_millihz=$TARGET_MILLIHZ"
} >"$RUN_DIR/automation-metadata.txt"

git -C "$ROOT" status --short --branch >"$RUN_DIR/git-status.txt"
ls -l "$VIDEO" "$CARD" >"$RUN_DIR/device-nodes.txt" 2>&1 || true

echo "=== Packaging results ==="
tar -czf "$ARCHIVE.tmp" -C "$RUN_DIR" .
mv -f "$ARCHIVE.tmp" "$ARCHIVE"
ln -f "$ARCHIVE" "$LATEST"
basename "$ARCHIVE" >"$RESULTS_BASE/latest-name.txt"
sha256sum "$ARCHIVE" | tee "$ARCHIVE.sha256"

echo
echo "RESULT ARCHIVE READY"
echo "  $ARCHIVE"
echo "  $LATEST"
echo
echo "On the Windows PC, from the repository root, run:"
echo "  powershell -ExecutionPolicy Bypass -File .\\tools\\fetch-latest.ps1"
echo
echo "Direct fallback:"
echo "  scp $(id -un)@$(hostname -I 2>/dev/null | awk '{print $1}'):~/hdmirx-results/latest.tar.gz \"\$HOME\\Downloads\\hdmirx-latest.tar.gz\""

if (( RUN_RC != 0 )); then
  echo "NOTE: the test exited $RUN_RC. The archive was still created for debugging." >&2
fi
exit "$RUN_RC"
