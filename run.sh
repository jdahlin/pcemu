#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")"
make -s
disk=media/win311.raw
if [[ ! -f "$disk" ]]; then disk=media/dos.raw; fi
if [[ ! -f "$disk" ]]; then
  echo 'No working disk. See README.md for media preparation.' >&2
  exit 1
fi
exec ./build/pcemu --disk "$disk" --type $'cd windows\nwin /s\n' "$@"
