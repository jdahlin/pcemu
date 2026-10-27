import importlib.util
import contextlib
import tempfile
from unittest.mock import patch
import io
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
    def test_jit_rejects_interpreter_only_profiling_before_boot(self):
        with tempfile.TemporaryDirectory() as directory:
            disk = Path(directory) / 'guest.qcow2'
            disk.touch()
            for options in [['--measure-window'], ['--profile-mode', 'counters'],
                            ['--profile-mode', 'host']]:
                with self.subTest(options=options), patch.object(sys, 'argv',
                        ['bench_quake', '--disk', str(disk), '--backend', 'jit'] + options), \
                        patch.object(bench.subprocess, 'run') as run, \
                        contextlib.redirect_stderr(io.StringIO()) as errors:
                    with self.assertRaises(SystemExit) as error:
                        bench.main()
                    self.assertEqual(error.exception.code, 2)
                    self.assertIn('requires --backend interpreter', errors.getvalue())
                    run.assert_not_called()

    def test_jit_coverage_reconciles_and_never_rounds_up_completion(self):
        report = dict(coverageGuestSteps=1000000000, coverageNativeSteps=999999999)
        coverage = bench.jit_coverage_result(report, dict(interpretedAttempts=1))
        self.assertFalse(coverage['complete'])
        self.assertEqual(coverage['interpretedSteps'], 1)
        report['coverageNativeSteps'] += 1
        self.assertTrue(bench.jit_coverage_result(report, dict(interpretedAttempts=0))['complete'])
        self.assertFalse(bench.jit_coverage_result(report, dict(interpretedAttempts=0,
            cpuTiming=dict(unknownAttempts=1)))['complete'])
        for total, native, attempts in [(0,0,0),(10,11,0),(10,9,0),(10,-1,11)]:
            with self.subTest(total=total, native=native), self.assertRaises(ValueError):
                bench.jit_coverage_result(dict(coverageGuestSteps=total, coverageNativeSteps=native),
                                          dict(interpretedAttempts=attempts))

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

    def test_short_demo_preserves_complete_packets(self):
        angles = struct.pack('<fff', 1, 2, 3)
        records = [struct.pack('<i', 2) + angles + bytes([1, n]) for n in range(65)]
        original = b'2\n' + b''.join(records) + struct.pack('<i', 1) + angles + b'\x02'
        short = bench.shorten_demo(original, 50)
        expected_prefix = b'2\n' + b''.join(records[:55])
        self.assertEqual(short, expected_prefix + struct.pack('<i', 1) + angles + b'\x02')
        self.assertEqual(len(original), 2 + 65 * 18 + 17)
        self.assertEqual(original[-1:], b'\x02')
        for count in [0, -1, 61]:
            with self.subTest(count=count), self.assertRaises(ValueError):
                bench.shorten_demo(original, count)

    def test_short_demo_rejects_damaged_recording(self):
        for demo in [b'bad', b'bad\n', b'-1\n' + bytes(15),
                     b'2\n' + struct.pack('<i', -1) + bytes(12),
                     b'2\n' + struct.pack('<i', 8001) + bytes(8013),
                     b'2\n' + struct.pack('<i', 2) + bytes(13)]:
            with self.subTest(demo=demo[:20]), self.assertRaises(ValueError):
                bench.shorten_demo(demo, 50)
        with self.assertRaises(ValueError):
            bench.shorten_demo(b'2\n' + struct.pack('<i', 1) + bytes(12) + b'\x01', 50)

    def test_extract_selected_demo(self):
        archive = pack([('demo1.dem', b'first'), ('DEMO2.DEM', b'second')])
        bench.pak_demos(archive)
        self.assertEqual(bench.demo_from_pak(archive, 'demo2'), b'second')
        with self.assertRaises(ValueError):
            bench.demo_from_pak(archive, 'missing')

    def test_add_owned_guest_file_and_preserve_existing(self):
        class Fat:
            cluster_bytes = 512
            database = 0
            entry = staticmethod(bench.Writer.entry)
            def __init__(self):
                self.f = io.BytesIO(bytes(2048))
                self.allocations = 0
            def entries(self): return [(0, 'ID1        ', 2, 0)]
            def chain(self, cluster): return iter([cluster])
            def read_file(self, cluster, size):
                self.f.seek((cluster - 2) * 512)
                return self.f.read(size)
            def allocate(self, data):
                self.allocations += 1
                self.f.seek(512)
                self.f.write(data.ljust(512, b'\0'))
                return 3
            def commit(self): pass
        fat = Fat()
        bench.put_id1_file(fat, 'PCBENCH.DEM', b'test demo')
        self.assertEqual(bench.read_guest_file(fat, 'ID1/PCBENCH.DEM'), b'test demo')
        self.assertEqual(fat.f.getvalue()[:11], b'PCBENCH DEM')
        before = fat.f.getvalue()
        with self.assertRaises(ValueError):
            bench.put_id1_file(fat, 'PCBENCH.DEM', b'replacement')
        self.assertEqual(fat.f.getvalue(), before)
        self.assertEqual(fat.allocations, 1)
        full = Fat()
        full.f.write(b'A' * 512)
        with self.assertRaises(ValueError):
            bench.put_id1_file(full, 'PCBENCH.CFG', b'timedemo pcbench')
        self.assertEqual(full.allocations, 0)

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
