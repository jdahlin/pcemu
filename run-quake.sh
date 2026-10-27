#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")"
disk="${QUAKE_DISK:-media/quake.qcow2}"
if [[ "${1:-}" == --profile ]]; then
  shift
  exec python3 tools/profile_quake.py --disk "$disk" "$@"
fi
if [[ "${1:-}" == --benchmark ]]; then
  shift
  exec python3 tools/bench_quake.py --disk "$disk" "$@"
fi
if [[ ! -f "$disk" ]]; then
  echo 'Prepare the DOS Quake disk first; see docs/quake.md.' >&2
  exit 1
fi
./tools/build.sh pcemu
# The interpreter is the verified Quake backend; CWSDPMI still exposes a JIT
# segment-state divergence. Do not silently select JIT through the auto backend.
exec ./build/pcemu --backend interpreter --ram-mb 32 --disk "$disk" "$@"
