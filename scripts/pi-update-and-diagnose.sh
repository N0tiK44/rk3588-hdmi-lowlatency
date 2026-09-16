#!/usr/bin/env bash
set -euo pipefail

ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
RESULT_DIR=${RESULT_DIR:-/tmp/hdmirx-v1}
ARCHIVE=${ARCHIVE:-$HOME/hdmirx-latest.tar.gz}

if (( EUID == 0 )); then
  echo "Run this updater as your normal user; it invokes sudo only for hardware access." >&2
  exit 2
fi

cd "$ROOT"

if [[ -n $(git status --porcelain) ]]; then
  echo "Refusing to overwrite local repository changes:" >&2
  git status --short >&2
  exit 2
fi

git switch main
git pull --ff-only
make clean
make check
make

set +e
sudo env \
  OUTDIR="$RESULT_DIR" \
  DURATION="${DURATION:-120}" \
  BUFFERS="${BUFFERS:-4}" \
  VIDEO="${VIDEO:-/dev/video0}" \
  CARD="${CARD:-/dev/dri/card0}" \
  CONNECTOR="${CONNECTOR:-217}" \
  PLANE="${PLANE:-114}" \
  TARGET_REFRESH_MILLIHZ="${TARGET_REFRESH_MILLIHZ:-59940}" \
  bash "$ROOT/scripts/diagnose.sh"
RUN_RC=$?
set -e

if [[ ! -d "$RESULT_DIR" ]]; then
  echo "No result directory was created." >&2
  exit 1
fi

tar -czf "$ARCHIVE" -C "$RESULT_DIR" .
sudo chown "$(id -u):$(id -g)" "$ARCHIVE" 2>/dev/null || true

echo
echo "Result archive: $ARCHIVE"
echo "Fetch from Windows PowerShell with:"
echo "  scp ${USER}@$(hostname -I | awk '{print $1}'):$ARCHIVE \"\$HOME\\Downloads\\hdmirx-latest.tar.gz\""
exit "$RUN_RC"
