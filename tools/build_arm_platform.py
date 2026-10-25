#!/usr/bin/env python3
"""Build a matching ARM CPU topology in ACPI and the firmware device tree."""
import argparse
from pathlib import Path
import subprocess
from build_arm_acpi import ROOT


def build(cpu_count, output, ram_mb=2048, hyperv=False, vmbus=False):
    if vmbus and not hyperv:
        raise ValueError('VMBus requires Hyper-V identity')
    if not 1 <= cpu_count <= 8:
        raise ValueError('ARM platform supports 1..8 CPUs')
    if not 256 <= ram_mb <= 16384:
        raise ValueError('ARM platform RAM must be 256..16384 MiB')
    output.mkdir(parents=True, exist_ok=True)
    subprocess.run(['python3', str(ROOT / 'tools/build_arm_acpi.py'), '--cpus', str(cpu_count),
                    '--output', str(output / 'acpi')] + (['--hyperv'] if hyperv else [])
                   + (['--vmbus'] if vmbus else []), check=True)
    source = (ROOT / 'platform/arm64/pcemu-virt.dts').read_text()
    original = 'cpu@0 { device_type = "cpu"; compatible = "arm,arm-v8"; reg = <0>; enable-method = "psci"; };'
    if source.count(original) != 1:
        raise ValueError('CPU device-tree template changed')
    cpus = '\n        '.join(original.replace('cpu@0', f'cpu@{i:x}').replace('reg = <0>', f'reg = <{i}>')
                             for i in range(cpu_count))
    redistributors = '0x080a0000 0 0x20000'
    if source.count(redistributors) != 1:
        raise ValueError('GIC device-tree template changed')
    source = source.replace(original, cpus).replace(redistributors, f'0x080a0000 0 0x{cpu_count * 0x20000:x}')
    memory = 'reg = <0 0x40000000 0 0x80000000>'
    if source.count(memory) != 1:
        raise ValueError('RAM device-tree template changed')
    ram_bytes = ram_mb * 1024 * 1024
    source = source.replace(memory, f'reg = <0 0x40000000 0x{ram_bytes >> 32:x} 0x{ram_bytes & 0xffffffff:x}>')
    dts = output / 'pcemu-virt.dts'
    dts.write_text(source)
    subprocess.run(['dtc', '-I', 'dts', '-O', 'dtb', '-o', str(output / 'pcemu-virt.dtb'), str(dts)], check=True)
    return output


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cpus', type=int, default=2, choices=range(1, 9))
    parser.add_argument('--ram-mb', type=int, default=2048)
    parser.add_argument('--hyperv', action='store_true')
    parser.add_argument('--vmbus', action='store_true')
    parser.add_argument('--output', type=Path)
    args = parser.parse_args()
    name = f'cpus{args.cpus}' + (f'-ram{args.ram_mb}' if args.ram_mb != 2048 else '')
    if args.hyperv:
        name += '-hyperv'
    if args.vmbus:
        name += '-vmbus'
    print(build(args.cpus, args.output or ROOT / 'build/arm64-fw' / name, args.ram_mb, args.hyperv, args.vmbus))


if __name__ == '__main__':
    main()
