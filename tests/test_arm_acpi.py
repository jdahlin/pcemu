"""Independently apply firmware relocations and inspect the resulting tables."""
from pathlib import Path
import struct
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
from build_arm_acpi import build, dsdt, TABLES, RSDP


class ArmAcpi(unittest.TestCase):
    def test_hda_intx_route(self):
        # AML Package(4) { DWord(0x8ffff), Zero, Zero, Byte(56) }:
        # slot 8 / any function, INTA, direct level interrupt GSI 56.
        self.assertIn(bytes.fromhex('120b040cffff080000000a38'), dsdt())

    def relocated(self, cpu_count=1, **options):
        tables, rsdp, script, _ = build(cpu_count, **options)
        files = {TABLES: bytearray(tables), RSDP: bytearray(rsdp)}
        addresses = {TABLES: 0x4e001000, RSDP: 0x4d000000}
        self.assertEqual(len(script) % 128, 0)
        allocated = set()
        for pos in range(0, len(script), 128):
            entry = script[pos:pos + 128]
            kind = struct.unpack_from('<I', entry)[0]
            name = entry[4:60].split(b'\0')[0].decode()
            if kind == 1:
                self.assertIn(name, files)
                alignment, zone = struct.unpack_from('<IB', entry, 60)
                self.assertEqual(addresses[name] % alignment, 0)
                self.assertEqual(zone, 1)
                self.assertNotIn(name, allocated)
                allocated.add(name)
            elif kind == 2:
                target = entry[60:116].split(b'\0')[0].decode()
                offset, width = struct.unpack_from('<IB', entry, 116)
                self.assertIn(name, allocated)
                self.assertIn(target, allocated)
                self.assertIn(width, (4, 8))
                self.assertLessEqual(offset + width, len(files[name]))
                relative = int.from_bytes(files[name][offset:offset + width], 'little')
                self.assertLess(relative, len(files[target]))
                files[name][offset:offset + width] = (addresses[target] + relative).to_bytes(width, 'little')
            elif kind == 3:
                result, start, length = struct.unpack_from('<III', entry, 60)
                self.assertLessEqual(start + length, len(files[name]))
                self.assertTrue(start <= result < start + length)
                self.assertEqual(files[name][result], 0)
                files[name][result] = -sum(files[name][start:start + length]) & 255
            else:
                self.fail(f'Unexpected loader operation {kind}')
        return files, addresses

    def test_relocated_graph(self):
        files, addresses = self.relocated()
        rsdp, blob = files[RSDP], files[TABLES]
        self.assertEqual(sum(rsdp[:20]) & 255, 0)
        self.assertEqual(sum(rsdp) & 255, 0)
        def table_at(address):
            offset = address - addresses[TABLES]
            self.assertGreaterEqual(offset, 0)
            length = struct.unpack_from('<I', blob, offset + 4)[0]
            self.assertGreaterEqual(length, 36)
            self.assertLessEqual(offset + length, len(blob))
            result = blob[offset:offset + length]
            self.assertEqual(sum(result) & 255, 0)
            return result
        xsdt = table_at(struct.unpack_from('<Q', rsdp, 24)[0])
        self.assertEqual(xsdt[:4], b'XSDT')
        self.assertEqual((len(xsdt) - 36) // 8, 7)
        tables = {}
        for pos in range(36, len(xsdt), 8):
            data = table_at(struct.unpack_from('<Q', xsdt, pos)[0])
            self.assertNotIn(bytes(data[:4]), tables)
            tables[bytes(data[:4])] = data
        self.assertEqual(set(tables), {b'FACP', b'APIC', b'GTDT', b'MCFG', b'SPCR', b'DBG2', b'PPTT'})
        fadt = tables[b'FACP']
        self.assertEqual(struct.unpack_from('<I', fadt, 112)[0], 1 << 20)
        self.assertEqual(struct.unpack_from('<H', fadt, 129)[0], 3)
        self.assertEqual(table_at(struct.unpack_from('<Q', fadt, 140)[0])[:4], b'DSDT')
        madt = tables[b'APIC']
        self.assertEqual(madt[44:46], bytes([11, 80]))
        self.assertEqual(struct.unpack_from('<II', madt, 44 + 8), (0, 1))
        self.assertEqual(struct.unpack_from('<Q', madt, 44 + 68)[0], 0)
        self.assertEqual(struct.unpack_from('<Q', madt, 44 + 80 + 8)[0], 0x08000000)
        self.assertEqual(struct.unpack_from('<QI', madt, 44 + 80 + 24 + 4), (0x080a0000, 0x20000))
        self.assertEqual(struct.unpack_from('<IIII', tables[b'GTDT'], 56), (30, 4, 27, 4))
        self.assertEqual(struct.unpack_from('<II', tables[b'GTDT'], 72), (0, 0))
        self.assertEqual(struct.unpack_from('<II', tables[b'GTDT'], 96), (0, 0))
        self.assertEqual(struct.unpack_from('<QHBB', tables[b'MCFG'], 44), (0x3f000000, 0, 0, 0))
        self.assertEqual(tables[b'SPCR'][36], 3)
        self.assertEqual(tables[b'SPCR'][52], 8)
        self.assertEqual(struct.unpack_from('<I', tables[b'SPCR'], 54)[0], 33)
        self.assertEqual(struct.unpack_from('<Q', tables[b'SPCR'], 44)[0], 0x09000000)

    def test_cpu_topology_variants(self):
        from build_arm_acpi import dsdt
        for count in (1, 2, 4, 8):
            with self.subTest(cpus=count):
                files, _ = self.relocated(count)
                blob, _, _, offsets = build(count)
                madt = files[TABLES][offsets['APIC']:offsets['APIC'] + 44 + 80 * count + 40]
                for cpu in range(count):
                    start = 44 + cpu * 80
                    self.assertEqual(madt[start:start + 2], bytes([11, 80]))
                    self.assertEqual(struct.unpack_from('<II', madt, start + 8), (cpu, 1))
                    self.assertEqual(struct.unpack_from('<Q', madt, start + 68)[0], cpu)
                gicr = 44 + count * 80 + 24
                self.assertEqual(struct.unpack_from('<QI', madt, gicr + 4), (0x080a0000, 0x20000 * count))
                pptt = files[TABLES][offsets['PPTT']:]
                self.assertEqual(pptt[36:40], bytes([0, 20, 0, 0]))
                self.assertEqual(struct.unpack_from('<IIII', pptt, 40), (1, 0, 0, 0))
                for cpu in range(count):
                    self.assertEqual(struct.unpack_from('<IIII', pptt, 60 + 20 * cpu), (10, 36, cpu, 0))
                namespace = dsdt(count)
                self.assertEqual(sum(namespace) & 255, 0)
                self.assertEqual(struct.unpack_from('<I', namespace, 4)[0], len(namespace))
                self.assertIn(struct.pack('<II', 0x080a0000, count * 0x20000), namespace)
                for cpu in range(count):
                    self.assertEqual(namespace.count(f'CPU{cpu}'.encode()), 1)
                self.assertNotIn(f'CPU{count}'.encode(), namespace)
        for count in (0, 9):
            with self.assertRaises(ValueError):
                build(count)

    def test_optional_vmbus_graph(self):
        from build_arm_acpi import vmbus_ssdt
        with self.assertRaises(ValueError):
            build(vmbus=True)
        for count in (1, 2, 8):
            files, addresses = self.relocated(count, hyperv=True, vmbus=True)
            _, _, _, offsets = build(count, hyperv=True, vmbus=True)
            blob = files[TABLES]
            ssdt = vmbus_ssdt()
            start = offsets['SSDT']
            self.assertEqual(blob[start:start + len(ssdt)], ssdt)
            self.assertEqual(sum(ssdt) & 255, 0)
            xsdt = offsets['XSDT']
            length = struct.unpack_from('<I', blob, xsdt + 4)[0]
            self.assertEqual(length, 36 + 8 * 8)
            roots = struct.unpack_from('<8Q', blob, xsdt + 36)
            self.assertIn(addresses[TABLES] + start, roots)
            self.assertEqual(sum(blob[xsdt:xsdt + length]) & 255, 0)
        self.assertNotIn('SSDT', build(hyperv=True)[3])
        self.assertNotIn('SSDT', build()[3])
        for name in (b'VMOD', b'VMBS', b'ACPI0004', b'VMBus', b'_STA', b'_DIS', b'_PS0', b'_PS3'):
            self.assertIn(name, ssdt)
        # Extended interrupt: consumer, edge, active-high, exclusive, one GSI16.
        descriptor = bytes([0x89, 6, 0, 3, 1, 16, 0, 0, 0])
        self.assertEqual(ssdt.count(descriptor), 1)
        self.assertNotIn(b'PCI0', ssdt)

    def test_debug_namespace_bounds(self):
        files, _ = self.relocated()
        blob, _, _, offsets = build()
        start = offsets['DBG2']
        length = struct.unpack_from('<I', blob, start + 4)[0]
        table = files[TABLES][start:start + length]
        device_offset, count = struct.unpack_from('<II', table, 36)
        self.assertEqual(count, 1)
        device = table[device_offset:]
        self.assertEqual(len(device), struct.unpack_from('<H', device, 1)[0])
        name_length, name_offset = struct.unpack_from('<HH', device, 4)
        self.assertEqual(device[name_offset:name_offset + name_length], b'\\_SB.COM0\0')


if __name__ == '__main__':
    unittest.main()
