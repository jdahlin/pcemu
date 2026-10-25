#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
if [ ! -f build/build.ninja ]; then
  cmake -S . -B build -G Ninja
fi
if [ "$#" -eq 0 ]; then set -- pcemu; fi
exec cmake --build build -- "$@"
