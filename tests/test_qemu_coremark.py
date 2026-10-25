"""Reject QEMU benchmark output with missing, wrong or failed guest checks."""
import importlib.util
from pathlib import Path
import unittest

spec = importlib.util.spec_from_file_location('qemu_bench',
    Path(__file__).resolve().parents[1] / 'tools/bench_qemu_coremark.py')
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)


class Validation(unittest.TestCase):
    text = ('seedcrc : 0xe9f5\nIterations : 15000\n[0]crclist : 0xe714\n'
            '[0]crcmatrix : 0x1fd7\n[0]crcstate : 0x8e3a\n')

    def test_canonical_checks(self):
        module.validate(self.text, 15000)

    def test_wrong_crc(self):
        with self.assertRaises(ValueError):
            module.validate(self.text.replace('e714', 'e715'), 15000)

    def test_wrong_iterations(self):
        with self.assertRaises(ValueError):
            module.validate(self.text, 15001)

    def test_guest_error(self):
        with self.assertRaises(ValueError):
            module.validate(self.text + '[0]ERROR! other validation failed', 15000)

    def test_known_missing_clock(self):
        module.validate(self.text + 'ERROR! Must execute for at least 10 secs for a valid result!', 15000)

    def test_other_error(self):
        with self.assertRaises(ValueError):
            module.validate(self.text + 'ERROR: allocation failed', 15000)

    def test_missing_crc(self):
        with self.assertRaises(ValueError):
            module.validate(self.text.split('[0]crcstate')[0], 15000)


if __name__ == '__main__':
    unittest.main()
