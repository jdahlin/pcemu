#!/bin/sh
# Start the installed minimal Debian ARM64 server; Ctrl-] saves and exits.
set -eu
cd "$(dirname "$0")"
vm_dir=${DEBIAN_VM_DIR:-"$PWD/media/debian-arm64"}
ssh_port=${DEBIAN_SSH_PORT:-2222}
if [ ! -f "$vm_dir/system.pcd" ] || [ ! -f "$vm_dir/base.raw" ]; then
    echo "No installed Debian disk in $vm_dir; see docs/debian-arm64.md" >&2
    exit 1
fi
make build/pcemu-hvf firmware-arm64
exec build/pcemu-hvf --machine arm64-uefi \
    --disk "$vm_dir/base.raw" --disk-overlay "$vm_dir/system.pcd" \
    --vars "$vm_dir/vars.bin" --network user --ssh-port "$ssh_port" \
    --run-forever --serial-stdio "$@"
