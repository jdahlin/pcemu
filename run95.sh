#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")"
./tools/build.sh
disk=media/win95.raw
if [[ ${1:-} == --vga ]]; then
  shift
elif [[ -f media/win95-monitor.pcd ]]; then
  disk=media/win95-monitor.pcd
elif [[ -f media/win95-svga.pcd ]]; then
  disk=media/win95-svga.pcd
fi
if [[ ! -f "$disk" ]]; then
  echo 'No Windows 95 working disk. See README.md for current media requirements.' >&2
  exit 1
fi
# Detect the contents, including when the historical .raw name is a symlink.
magic=$(od -An -tx1 -N4 "$disk" | tr -d ' \n')
if [[ "$magic" == 514649fb ]]; then
  disk=media/win95-working.pcd
  if [[ ! -e "$disk" ]]; then
    ./tools/build.sh disk-tool
    ./build/disk-tool overlay "$disk" media/win95.raw
  fi
fi
exec ./build/pcemu --machine win95 --disk "$disk" --ram-mb 64 --execute-vgabios "$@"
