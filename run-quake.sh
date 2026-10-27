#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")"
./tools/build.sh pcemu
disk="${QUAKE_DISK:-media/quake.qcow2}"
if [[ ! -f "$disk" ]]; then
  echo 'Prepare the DOS Quake disk first; see docs/quake.md.' >&2
  exit 1
fi
# The interpreter is the verified Quake backend; CWSDPMI still exposes a JIT
# segment-state divergence. Do not silently select JIT through the auto backend.
exec ./build/pcemu --backend interpreter --ram-mb 32 --disk "$disk" "$@"
