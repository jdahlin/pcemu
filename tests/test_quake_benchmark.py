import importlib.util
import struct
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
spec = importlib.util.spec_from_file_location('bench_quake', ROOT / 'tools/bench_quake.py')
bench = importlib.util.module_from_spec(spec)
spec.loader.exec_module(bench)


def pack(entries):
    data = bytearray(b'PACK' + bytes(8))
    directory = bytearray()
    for name, payload in entries:
        directory += struct.pack('<56sII', name.encode('ascii'), len(data), len(payload))
        data += payload
    struct.pack_into('<II', data, 4, len(data), len(directory))
    return bytes(data + directory)


class QuakeBenchmarkTests(unittest.TestCase):
    def test_find_bundled_demos(self):
        self.assertEqual(bench.pak_demos(pack([('demo2.dem', b'2'), ('DEMO1.DEM', b'1'),
            ('maps/e1m1.bsp', b'map'), ('subdir/demo3.dem', b'x')])), ['demo1', 'demo2'])

    def test_archive_validation(self):
        for data in [b'', b'PACK' + struct.pack('<II', 12, 64), b'NOPE' + bytes(8),
                     b'PACK' + struct.pack('<II', 12, 1) + b'x', pack([('readme.txt', b'x')])]:
            with self.subTest(data=data), self.assertRaises(ValueError):
                bench.pak_demos(data)
        bad = bytearray(pack([('demo1.dem', b'demo')]))
        struct.pack_into('<I', bad, len(bad) - 4, 1000000)
        with self.assertRaises(ValueError):
            bench.pak_demos(bad)

    def test_offline_nested_file_read(self):
        # Literal FAT directory layout: first cluster is a directory, next a file.
        directory = bytearray(512)
        directory[:11] = b'PAK0    PAK'
        directory[11] = 0x20
        struct.pack_into('<HI', directory, 26, 3, 4)
        class Fat:
            cluster_bytes = 512
            def entries(self): return [(0, 'ID1        ', 2, 0)]
            def chain(self, cluster): return iter([cluster])
            def read_file(self, cluster, size): return {2: bytes(directory), 3: b'PACK'}[cluster][:size]
        self.assertEqual(bench.read_guest_file(Fat(), 'id1/pak0.pak'), b'PACK')
        with self.assertRaises(ValueError):
            bench.read_guest_file(Fat(), 'id1/missing.dem')


if __name__ == '__main__':
    unittest.main()
