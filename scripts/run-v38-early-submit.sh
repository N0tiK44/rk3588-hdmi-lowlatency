#!/usr/bin/env bash
set -euo pipefail

echo "ERROR: V3.8 is retired: its intentional phase-prime drop added one frame." >&2
echo "Run V3.8.1 instead: bash scripts/pi-debug.sh v381" >&2
exit 2
