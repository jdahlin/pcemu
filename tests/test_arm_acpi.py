"""Independently apply firmware relocations and inspect the resulting tables."""
from pathlib import Path
import struct
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
from build_arm_acpi import build, TABLES, RSDP


class ArmAcpi(unittest.TestCase):
    def relocated(self):
        tables, rsdp, script, _ = build()
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
        self.assertEqual((len(xsdt) - 36) // 8, 6)
        tables = {}
        for pos in range(36, len(xsdt), 8):
            data = table_at(struct.unpack_from('<Q', xsdt, pos)[0])
            self.assertNotIn(bytes(data[:4]), tables)
            tables[bytes(data[:4])] = data
        self.assertEqual(set(tables), {b'FACP', b'APIC', b'GTDT', b'MCFG', b'SPCR', b'DBG2'})
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
        self.assertEqual(struct.unpack_from('<QHBB', tables[b'MCFG'], 44), (0x3f000000, 0, 0, 0))
        self.assertEqual(tables[b'SPCR'][36], 3)
        self.assertEqual(struct.unpack_from('<Q', tables[b'SPCR'], 44)[0], 0x09000000)

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
