#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")"
./tools/build.sh
point=build/snapshots/office97-networked
if [[ ! -f "$point/state.bin" ]]; then point=build/snapshots/office97-integrated; fi
if [[ ! -f "$point/state.bin" ]]; then point=build/snapshots/office97-desktop; fi
case ${1:-} in
    --setup) point=build/snapshots/office97-setup; shift ;;
    --integrated) point=build/snapshots/office97-integrated; shift ;;
    --vga) point=build/snapshots/office97-desktop; shift ;;
esac
if [[ ! -f "$point/state.bin" ]]; then
    echo "Missing Office 97 checkpoint: $point. See docs/office97.md." >&2
    exit 1
fi
exec ./build/pcemu --machine win95 --load-snapshot "$point" "$@"
