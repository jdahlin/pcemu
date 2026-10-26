"""Recycled addresses must resolve by code lifetime, with boundary uncertainty."""
import json
from pathlib import Path
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
from jit_debug_map import TimedCodeMap


def load(time, generation, ip=0):
    return dict(event='load', timeNs=time, generation=generation, start=4096, end=4104,
                arm64='0000000000000000', cr3=0, cs=8, ip=ip,
                regions=[dict(offset=0, kind='Prologue', guestIP=ip),
                         dict(offset=4, kind='Alu', guestIP=ip)])


class JitDebugMapTests(unittest.TestCase):
    def mapping(self, directory, events, version=1):
        path = Path(directory)/'map.jsonl'
        path.write_text('\n'.join(json.dumps(e) for e in
            [dict(event='header', version=version, clock='unix-ns', pid=42), *events]))
        return TimedCodeMap(path)

    def test_reuse_never_attributes_old_samples_to_new_code(self):
        with tempfile.TemporaryDirectory() as directory:
            mapping = self.mapping(directory, [
                dict(event='reset', timeNs=0, generation=1), load(2_000_000, 1, 16),
                dict(event='reset', timeNs=10_000_000, generation=2), load(11_000_000, 2, 32)])
            self.assertEqual(mapping.lookup(4100, 5_000_000)[0]['ip'], 16)
            self.assertEqual(mapping.lookup(4100, 15_000_000)[0]['ip'], 32)
            self.assertEqual(mapping.lookup(4100, 15_000_000)[1]['kind'], 'Alu')
            for time in (1_000_000, 2_000_000, 9_500_000, 10_500_000, 11_500_000):
                self.assertIsNone(mapping.lookup(4100, time))
            self.assertIsNone(mapping.lookup(4104, 15_000_000))
            self.assertIsNone(mapping.lookup(4095, 15_000_000))

    def test_branch_patches_keep_regions_and_require_live_code(self):
        with tempfile.TemporaryDirectory() as directory:
            reset = dict(event='reset', timeNs=0, generation=1)
            patch = dict(event='patch', timeNs=4_000_000, generation=1,
                         start=4100, end=4104, arm64='00000014')
            mapping = self.mapping(directory, [reset, load(1_000_000, 1), patch], version=2)
            self.assertEqual(mapping.lookup(4100, 5_000_000)[1]['kind'], 'Alu')
            patch['start'], patch['end'] = 4104, 4108
            with self.assertRaisesRegex(ValueError, 'Invalid JIT patch'):
                self.mapping(directory, [reset, load(1_000_000, 1), patch], version=2)

    def test_overlapping_live_code_and_bad_machine_bytes_rejected(self):
        with tempfile.TemporaryDirectory() as directory:
            reset = dict(event='reset', timeNs=0, generation=1)
            with self.assertRaisesRegex(ValueError, 'Overlapping'):
                self.mapping(directory, [reset, load(1, 1), load(2, 1)])
            bad = load(1, 1)
            bad['arm64'] = '00'
            with self.assertRaisesRegex(ValueError, 'Invalid JIT code'):
                self.mapping(directory, [reset, bad])


if __name__ == '__main__':
    unittest.main()
