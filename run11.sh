#!/bin/sh
# Start the installed Windows 11 ARM64 guest in SDL.
set -eu
cd "$(dirname "$0")"

if [ "${1:-}" = --help ]; then
    echo 'Usage: ./run11.sh [additional pcemu-hvf options]'
    echo 'Starts Windows in SDL with 4 GiB RAM, 2 CPUs, USB keyboard/mouse and user networking.'
    echo 'Click to capture input; Cmd+G releases it. Shut down from Windows before closing.'
    exit 0
fi

for file in media/windows11-arm64/system.qcow2 \
            media/windows11-arm64/system-live.qcow2 \
            build/windows11-qcow2/installed-vars.bin; do
    if [ ! -f "$file" ]; then
        echo "Missing installed Windows file: $file" >&2
        exit 1
    fi
done

./tools/build.sh pcemu-hvf
python3 tools/build_hvf_app.py
python3 tools/build_arm_platform.py --cpus 2 --ram-mb 4096 --hyperv --vmbus --msi
mkdir -p build/windows11-sdl
echo 'Starting Windows. Click to capture input; Cmd+G releases it.'
exec ./build/PCEmu11.app/Contents/MacOS/pcemu-hvf --machine arm64-uefi --gui --run-forever \
    --ram-mb 4096 --cpus 2 --hyperv vmbus --network user --virtio-msix \
    --disk-controller virtio --virtio-transport pci \
    --disk media/windows11-arm64/system.qcow2 \
    --disk-overlay media/windows11-arm64/system-live.qcow2 \
    --vars build/windows11-qcow2/installed-vars.bin --cpu-state \
    --hyperv-stats build/windows11-sdl/hyperv.json \
    --framebuffer-ppm build/windows11-sdl/framebuffer.ppm "$@"
