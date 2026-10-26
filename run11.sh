#!/bin/sh
# Start installed Windows 11 ARM64 locally or through SPICE.
set -eu
cd "$(dirname "$0")"

if [ "${1:-}" = --help ]; then
    echo 'Usage: ./run11.sh [--spice] [additional pcemu-hvf options]'
    echo '--spice serves the display on 127.0.0.1:5930 for a SPICE client.'
    echo 'Starts Windows in SDL with 4 GiB RAM, 2 CPUs, VirtIO GPU, serial, balloon, USB input, ICH9 audio and user networking.'
    echo 'Uses a seamless absolute mouse and native host cursor. Shut down inside Windows.'
    exit 0
fi

ram_mb=${PCEMU_RAM_MB:-4096}
cpus=${PCEMU_CPUS:-2}
frontend=--gui
if [ "${1:-}" = --spice ]; then
    shift
    set -- --spice-port "${PCEMU_SPICE_PORT:-5930}" "$@"
    frontend=
fi
for option in "$@"; do
    if [ "$option" = --spice-port ]; then frontend=; fi
done

for file in media/windows11-arm64/system.qcow2 \
            media/windows11-arm64/system-spice-live.qcow2 \
            build/windows11-qcow2/installed-vars.bin; do
    if [ ! -f "$file" ]; then
        echo "Missing installed Windows file: $file" >&2
        exit 1
    fi
done

./tools/build.sh pcemu-hvf
python3 tools/build_hvf_app.py
python3 tools/build_arm_platform.py --cpus "$cpus" --ram-mb "$ram_mb" --hyperv --vmbus --msi
mkdir -p build/windows11-sdl
echo 'Starting Windows with seamless mouse and host cursor.'
exec ./build/PCEmu11.app/Contents/MacOS/pcemu-hvf --machine arm64-uefi $frontend --run-forever \
    --ram-mb "$ram_mb" --cpus "$cpus" --hyperv vmbus --network user --virtio-msix \
    --disk-controller virtio --virtio-transport pci \
    --virtio-gpu --virtio-serial --virtio-balloon --balloon-pfn64 --audio ich9 \
    --disk media/windows11-arm64/system.qcow2 \
    --disk-overlay media/windows11-arm64/system-spice-live.qcow2 \
    --vars build/windows11-qcow2/installed-vars.bin --cpu-state \
    --hyperv-stats build/windows11-sdl/hyperv.json \
    --framebuffer-ppm build/windows11-sdl/framebuffer.ppm "$@"
