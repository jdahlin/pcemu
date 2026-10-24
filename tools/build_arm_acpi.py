#!/usr/bin/env python3
"""Build original ACPI tables and the firmware's documented relocation script.

The checked-in AML is compiled from our ASL; its source hash prevents stale AML.
Pass --iasl to additionally recompile and compare it with the checked-in bytes.
"""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import subprocess

ROOT = Path(__file__).resolve().parents[1]
TABLES = 'etc/acpi/tables'
RSDP = 'etc/acpi/rsdp'


def table(signature, revision, body):
    data = bytearray(struct.pack('<4sIBB6s8sI4sI', signature.encode(), 36 + len(body),
                                 revision, 0, b'PCEMU ', b'ARMVIRT ', 1, b'PCEM', 1) + body)
    data[9] = -sum(data) & 255
    return data


def filename(name):
    return name.encode().ljust(56, b'\0')


def command(kind, payload):
    if len(payload) > 124:
        raise ValueError('Oversized table-loader command')
    return struct.pack('<I', kind) + payload.ljust(124, b'\0')


def allocate(name):
    return command(1, filename(name) + struct.pack('<IB', 64, 1))


def pointer(source, target, offset, width=8):
    return command(2, filename(source) + filename(target) + struct.pack('<IB', offset, width))


def checksum(name, result, start, length):
    return command(3, filename(name) + struct.pack('<III', result, start, length))


def aml_package(opcode, body):
    for width in range(1, 5):
        length = len(body) + width
        if length < (1 << (6 if width == 1 else 4 + 8 * (width - 1))):
            encoded = bytes([length]) if width == 1 else bytes([
                ((width - 1) << 6) | (length & 15),
                *[(length >> (4 + 8 * i)) & 255 for i in range(width - 1)]])
            return opcode + encoded + body
    raise ValueError('AML package too large')


def dsdt(cpu_count=1):
    source = ROOT / 'platform/arm64/dsdt.asl'
    cached = json.loads((source.parent / 'dsdt-aml.json').read_text())
    if hashlib.sha256(source.read_bytes()).hexdigest() != cached['sourceSha256']:
        raise ValueError('DSDT source changed: recompile and update dsdt-aml.json')
    data = bytes.fromhex(cached['amlHex'])
    if data[:4] != b'DSDT' or len(data) != struct.unpack_from('<I', data, 4)[0] or sum(data) & 255:
        raise ValueError('Invalid compiled DSDT')
    if cpu_count == 1:
        return data
    # Extend the single top-level _SB scope with simple processor devices.
    # The source hash above still validates the original compiled namespace.
    if data[36] != 0x10:
        raise ValueError('Expected a top-level AML Scope')
    width = 1 + (data[37] >> 6)
    length = data[37] & (63 if width == 1 else 15)
    for i in range(width - 1):
        length |= data[38 + i] << (4 + 8 * i)
    if 37 + length != len(data) or data[37 + width:41 + width] != b'_SB_':
        raise ValueError('Expected exactly one top-level _SB scope')
    body = bytearray(data[37 + width:])
    resource = struct.pack('<II', 0x080a0000, 0x20000)
    if body.count(resource) != 1:
        raise ValueError('Expected one GIC redistributor resource')
    body = body.replace(resource, struct.pack('<II', 0x080a0000, 0x20000 * cpu_count))
    for cpu in range(1, cpu_count):
        device = f'CPU{cpu}'.encode() + b'\x08_HID\x0dACPI0007\0\x08_UID\x0a' + bytes([cpu])
        body.extend(aml_package(b'\x5b\x82', device))
    return table('DSDT', 2, aml_package(b'\x10', body))


def gas(address):
    return struct.pack('<BBBBQ', 0, 32, 0, 3, address)


def build(cpu_count=1):
    if not 1 <= cpu_count <= 8:
        raise ValueError('ARM platform supports 1..8 CPUs')
    fadt = bytearray(276 - 36)
    struct.pack_into('<I', fadt, 112 - 36, 1 << 20)  # hardware-reduced ACPI
    struct.pack_into('<H', fadt, 129 - 36, 3)  # PSCI compliant, HVC conduit
    fadt[131 - 36] = 5  # ACPI 6.5 minor revision

    giccs = bytearray()
    for cpu in range(cpu_count):
        gicc = bytearray(80)
        struct.pack_into('<BBHIII', gicc, 0, 11, 80, 0, cpu, cpu, 1)
        struct.pack_into('<Q', gicc, 68, cpu)
        giccs.extend(gicc)
    # No PMU IRQ or GICv2 interface. Redistributors follow creation/affinity order.
    gicd = struct.pack('<BBHIQIB3x', 12, 24, 0, 0, 0x08000000, 0, 3)
    gicr = struct.pack('<BBHQI', 14, 16, 0, 0x080a0000, 0x20000 * cpu_count)
    madt = struct.pack('<II', 0, 0) + giccs + gicd + gicr
    # One virtual package with distinct cores; no invented physical cache data.
    pptt = struct.pack('<BBHIIII', 0, 20, 0, 1, 0, 0, 0)
    for cpu in range(cpu_count):
        pptt += struct.pack('<BBHIIII', 0, 20, 0, 0x0a, 36, cpu, 0)

    gtdt = bytearray(104 - 36)
    struct.pack_into('<Q', gtdt, 0, 0xffffffffffffffff)  # no MMIO counter control
    struct.pack_into('<IIII', gtdt, 56 - 36, 30, 4, 27, 4)  # level-high, always-on
    # EL2 timer fields stay zero: HVF does not provide that timer with EL2 off.
    # Linux 6.12 still attempts to map GSI0; inventing PPI 26 would hide a real
    # capability mismatch. EL1 virtual/physical timers remain available.
    struct.pack_into('<Q', gtdt, 80 - 36, 0xffffffffffffffff)  # no MMIO counter read

    mcfg = bytes(8) + struct.pack('<QHBBI', 0x3f000000, 0, 0, 0, 0)
    spcr = bytearray(80 - 36)
    spcr[0] = 3  # PL011
    spcr[52 - 36] = 8  # ARM GIC interrupt
    struct.pack_into('<I', spcr, 54 - 36, 33)
    spcr[4:16] = gas(0x09000000)
    spcr[58 - 36] = 7  # 115200 baud
    spcr[60 - 36] = 1  # one stop bit
    spcr[62 - 36] = 3  # ANSI terminal
    struct.pack_into('<HH', spcr, 64 - 36, 0xffff, 0xffff)

    namespace = b'\\_SB.COM0\0'
    device = struct.pack('<BHBHHHHHH2xHH', 0, 22 + 12 + 4 + len(namespace), 1,
                         len(namespace), 38, 0, 0, 0x8000, 3, 22, 34)
    dbg2 = struct.pack('<II', 44, 1) + device + gas(0x09000000) + struct.pack('<I', 4096) + namespace

    entries = [('DSDT', dsdt(cpu_count)), ('FACP', table('FACP', 6, fadt)),
               ('APIC', table('APIC', 5, madt)), ('GTDT', table('GTDT', 3, gtdt)),
               ('MCFG', table('MCFG', 1, mcfg)), ('SPCR', table('SPCR', 2, spcr)),
               ('DBG2', table('DBG2', 0, dbg2)), ('PPTT', table('PPTT', 3, pptt))]
    blob = bytearray()
    offsets = {}
    for name, data in entries:
        blob.extend(bytes(-len(blob) % 8))
        offsets[name] = len(blob)
        blob.extend(data)
    blob.extend(bytes(-len(blob) % 8))
    offsets['XSDT'] = len(blob)
    roots = [name for name, _ in entries if name != 'DSDT']
    xsdt = table('XSDT', 1, b''.join(struct.pack('<Q', offsets[name]) for name in roots))
    blob.extend(xsdt)
    struct.pack_into('<Q', blob, offsets['FACP'] + 140, offsets['DSDT'])
    # The firmware patches these relative pointers, fixes checksums, then
    # installs the pointed-to tables through EFI_ACPI_TABLE_PROTOCOL. Firmware
    # creates the final RSDP/XSDT and also fixes FADT's DSDT address after copying.
    rsdp = bytearray(36)
    struct.pack_into('<8sB6sBIIQ', rsdp, 0, b'RSD PTR ', 0, b'PCEMU ', 2, 0, 36, offsets['XSDT'])
    loader = bytearray(allocate(TABLES) + allocate(RSDP))
    loader.extend(pointer(TABLES, TABLES, offsets['FACP'] + 140))
    for i, name in enumerate(roots):
        loader.extend(pointer(TABLES, TABLES, offsets['XSDT'] + 36 + i * 8))
    loader.extend(pointer(RSDP, TABLES, 24))
    for name, data in entries + [('XSDT', xsdt)]:
        # Clear the old checksum before asking firmware to compute a new one.
        blob[offsets[name] + 9] = 0
        loader.extend(checksum(TABLES, offsets[name] + 9, offsets[name], len(data)))
    loader.extend(checksum(RSDP, 8, 0, 20))
    loader.extend(checksum(RSDP, 32, 0, 36))
    return bytes(blob), bytes(rsdp), bytes(loader), offsets


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, default=ROOT / 'build/arm64-fw/acpi')
    parser.add_argument('--iasl', type=Path)
    parser.add_argument('--cpus', type=int, default=1, choices=range(1, 9))
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    if args.iasl:
        prefix = args.output / 'checked-dsdt'
        subprocess.run([str(args.iasl), '-p', str(prefix), str(ROOT / 'platform/arm64/dsdt.asl')], check=True)
        if prefix.with_suffix('.aml').read_bytes() != dsdt():
            raise ValueError('Recompiled DSDT differs from dsdt-aml.json')
    blob, rsdp, loader, offsets = build(args.cpus)
    for name, data in [('tables', blob), ('rsdp', rsdp), ('table-loader', loader)]:
        (args.output / name).write_bytes(data)
    (args.output / 'manifest.json').write_text(json.dumps({'cpuCount': args.cpus, 'tableOffsets': offsets,
        'tableBytes': len(blob), 'loaderBytes': len(loader)}, indent=2) + '\n')
    print(f'Built ARM ACPI: {len(blob)} table bytes, {len(loader)} loader bytes')


if __name__ == '__main__':
    main()
