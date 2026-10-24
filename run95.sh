#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")"
make -s
if [[ ! -f media/win95.raw ]]; then
  echo 'No Windows 95 working disk. See README.md for current media requirements.' >&2
  exit 1
fi
exec ./build/pcemu --disk media/win95.raw --ram-mb 64 --execute-vgabios "$@"
