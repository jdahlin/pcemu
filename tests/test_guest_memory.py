"""Independent host pointer oracle rejects incomplete or wrong guest sweeps."""
import sys
import unittest
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
from bench_guest_memory import CASES, permutation, validate


def transcript(loops):
    return '\n'.join(f'{p:08x} {s:08x} {n:08x} '
                     f'{0x400000 + permutation(n)[(loops * 32 + 7) % n] * s:08x}'
                     for p, s, n in CASES)


class MemorySweep(unittest.TestCase):
    def test_permutations_cover_every_node(self):
        for n in (16, 256, 1024, 2048, 4096):
            self.assertEqual(sorted(permutation(n)), list(range(n)))
        self.assertEqual(permutation(16), [15, 14, 13, 10, 3, 2, 1, 9, 12, 7, 8, 4, 0, 6, 11, 5])

    def test_all_case_results_and_units(self):
        records = validate(transcript(257), 257)
        self.assertEqual(len(records), 20)
        self.assertEqual(records[-1]['guestPagesTouched'], 4096)
        self.assertEqual(records[4]['guestPagesTouched'], 4)
        self.assertEqual(records[0]['dependentLoads'], 8231)

    def test_wrong_pointer(self):
        lines = transcript(257).splitlines()
        lines[0] = '00000000 00000004 00000010 00400000'
        with self.assertRaises(ValueError): validate('\n'.join(lines), 257)

    def test_wrong_load_count(self):
        with self.assertRaises(ValueError): validate(transcript(257), 258)

    def test_missing_extra_and_wrong_order(self):
        lines = transcript(257).splitlines()
        for changed in (lines[:-1], lines + [lines[0]], lines[::-1]):
            with self.assertRaises(ValueError): validate('\n'.join(changed), 257)

    def test_malformed_fields(self):
        for text in (transcript(257).replace('00000000', 'xyz', 1),
                     transcript(257).replace('00000000', '0 1', 1)):
            with self.assertRaises(ValueError): validate(text, 257)


if __name__ == '__main__': unittest.main()
