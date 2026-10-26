#!/bin/sh
# Start the installed minimal Debian ARM64 server; Ctrl-] saves and exits.
set -eu
cd "$(dirname "$0")"
vm_dir=${DEBIAN_VM_DIR:-"$PWD/media/debian-arm64"}
ssh_port=${DEBIAN_SSH_PORT:-2222}
cpus=${DEBIAN_CPUS:-2}
expect_cpu_count=0
for argument in "$@"; do
    if [ "$expect_cpu_count" = 1 ]; then cpus=$argument; expect_cpu_count=0; fi
    if [ "$argument" = --cpus ]; then expect_cpu_count=1; fi
done
if [ ! -f "$vm_dir/system.qcow2" ] || [ ! -f "$vm_dir/base.raw" ]; then
    echo "No installed Debian disk in $vm_dir; see docs/debian-arm64.md" >&2
    exit 1
fi
make build/pcemu-hvf firmware-arm64
python3 tools/build_arm_platform.py --cpus "$cpus"
exec build/pcemu-hvf --machine arm64-uefi --cpus "$cpus" \
    --disk "$vm_dir/base.raw" --disk-overlay "$vm_dir/system.qcow2" \
    --vars "$vm_dir/vars.bin" --network user --ssh-port "$ssh_port" \
    --run-forever --serial-stdio "$@"
