"""Verify installer disk-selection refusal paths using a read-only WinAPI fixture."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest
ROOT = Path(__file__).resolve().parents[1]


class DiskGuardTest(unittest.TestCase):
    @unittest.skipUnless(shutil.which('clang'), 'clang is needed for the native WinAPI fixture')
    def test_disk_guard_refuses_mismatched_or_nonempty_disks(self):
        with tempfile.TemporaryDirectory() as tmp:
            executable = Path(tmp) / 'disk-guard-test'
            subprocess.run(['clang', '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror',
                            str(ROOT / 'tests/windows_disk_guard_test.c'), '-o', str(executable)], check=True)
            subprocess.run([str(executable)], check=True)


if __name__ == '__main__':
    unittest.main()
