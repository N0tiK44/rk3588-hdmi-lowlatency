#!/usr/bin/env bash
set -euo pipefail

# Small bootstrap kept separate from the runner so a pull can safely replace the
# worker before it is executed.
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
EXPERIMENT="${1:-v381}"

usage() {
  cat <<'EOF'
Usage: bash scripts/pi-debug.sh [baseline|v35|v36|v37|v38|v381]

From the existing Orange Pi checkout this performs a safe fast-forward update,
builds and checks the project, runs the selected hardware test, and creates:

  ~/hdmirx-results/latest.tar.gz

Run this as the normal login user, not through sudo. The worker asks for sudo
only when device access is required.
EOF
}

if [[ "$EXPERIMENT" == "-h" || "$EXPERIMENT" == "--help" ]]; then
  usage
  exit 0
fi

case "$EXPERIMENT" in
  baseline|v35|v36|v37|v38|v381) ;;
  *)
    echo "ERROR: unknown experiment '$EXPERIMENT'." >&2
    usage >&2
    exit 2
    ;;
esac

if (( EUID == 0 )); then
  echo "ERROR: do not run this wrapper with sudo." >&2
  echo "Run: bash scripts/pi-debug.sh $EXPERIMENT" >&2
  exit 2
fi

if ! git -C "$ROOT" rev-parse --is-inside-work-tree >/dev/null 2>&1; then
  echo "ERROR: $ROOT is not a Git checkout." >&2
  exit 1
fi

if [[ -n "$(git -C "$ROOT" status --porcelain)" ]]; then
  echo "ERROR: the checkout contains local changes; nothing was overwritten." >&2
  git -C "$ROOT" status --short >&2
  echo "Commit, preserve, or discard those changes deliberately, then retry." >&2
  exit 1
fi

echo "=== Updating the Orange Pi checkout ==="
git -C "$ROOT" switch main
git -C "$ROOT" pull --ff-only origin main

# Reload the worker after the pull so this run always uses the newest workflow.
exec bash "$ROOT/scripts/pi-run-and-package.sh" "$EXPERIMENT"
