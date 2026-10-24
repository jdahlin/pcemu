"""Explicit VirtIO CNG provider selection and failure-path fixtures."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest
ROOT = Path(__file__).resolve().parents[1]


class RngTest(unittest.TestCase):
    @unittest.skipUnless(shutil.which('clang'), 'clang is needed for the native CNG fixture')
    def test_rng_requires_successful_provider_io(self):
        with tempfile.TemporaryDirectory() as tmp:
            executable = Path(tmp) / 'rng-test'
            subprocess.run(['clang', '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror',
                            str(ROOT / 'tests/windows_rng_test.c'), '-o', str(executable)], check=True)
            subprocess.run([str(executable)], check=True)


if __name__ == '__main__':
    unittest.main()
