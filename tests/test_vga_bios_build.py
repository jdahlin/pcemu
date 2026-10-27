"""Offline font provenance, encoding and reproducible ROM packaging checks."""
import hashlib
from pathlib import Path
import shutil
import struct
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
from build_vga_bios import build, font_bytes


class VgaBiosBuild(unittest.TestCase):
    def test_font_mapping(self):
        font = font_bytes()
        self.assertEqual(len(font), 256 * 8)
        self.assertEqual(font[65*8:66*8], bytes([0x30,0x78,0xcc,0xcc,0xfc,0xcc,0xcc,0]))
        self.assertEqual(font[196*8:197*8], bytes([0,0,0,0,255,0,0,0]))  # CP437 horizontal line
        self.assertNotEqual(font[254*8:255*8], bytes(8))  # original supplementary square

    @unittest.skipUnless(shutil.which('nasm'), 'NASM required')
    def test_reproducible_header_and_checksum(self):
        with tempfile.TemporaryDirectory() as folder:
            first, second = (Path(folder) / name / 'vga.bin' for name in ('a', 'b'))
            build(first)
            build(second)
            data = first.read_bytes()
            self.assertEqual(data, second.read_bytes())
            embedded = (first.parent / 'pcemu-vga-bytes.inc').read_text()
            self.assertEqual(bytes(int(value,16) for value in embedded.replace('\n','').split(',') if value),data)
            self.assertEqual(embedded,(second.parent / 'pcemu-vga-bytes.inc').read_text())
            self.assertEqual(len(data), 32768)
            self.assertEqual(data[:3], b'\x55\xaa\x40')
            self.assertEqual(sum(data) & 255, 0)
            pcir = struct.unpack_from('<H', data, 24)[0]
            self.assertEqual(data[pcir:pcir+4], b'PCIR')
            self.assertEqual(struct.unpack_from('<HH', data, pcir+4), (0x1234, 0x1111))
            self.assertEqual(struct.unpack_from('<H', data, pcir+16)[0] * 512, len(data))
            self.assertEqual(data[pcir+20:pcir+22], b'\x00\x80')
            corrupt = bytearray(data)
            corrupt[1024] ^= 1
            self.assertNotEqual(sum(corrupt) & 255, 0)
            self.assertEqual(hashlib.sha256(data).digest(), hashlib.sha256(second.read_bytes()).digest())


if __name__ == '__main__':
    unittest.main()
