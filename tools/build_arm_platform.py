#!/usr/bin/env python3
"""Build a matching ARM CPU topology in ACPI and the firmware device tree."""
import argparse
from pathlib import Path
import subprocess
from build_arm_acpi import ROOT


def build(cpu_count, output):
    if not 1 <= cpu_count <= 8:
        raise ValueError('ARM platform supports 1..8 CPUs')
    output.mkdir(parents=True, exist_ok=True)
    subprocess.run(['python3', str(ROOT / 'tools/build_arm_acpi.py'), '--cpus', str(cpu_count),
                    '--output', str(output / 'acpi')], check=True)
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
    dts = output / 'pcemu-virt.dts'
    dts.write_text(source)
    subprocess.run(['dtc', '-I', 'dts', '-O', 'dtb', '-o', str(output / 'pcemu-virt.dtb'), str(dts)], check=True)
    return output


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cpus', type=int, default=2, choices=range(1, 9))
    parser.add_argument('--output', type=Path)
    args = parser.parse_args()
    print(build(args.cpus, args.output or ROOT / f'build/arm64-fw/cpus{args.cpus}'))


if __name__ == '__main__':
    main()
