"""Integration checks for the signed Apple Silicon HVF runner."""
import fcntl
from pathlib import Path
import struct
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class HvfCLI(unittest.TestCase):
    def invoke(self, *args):
        return subprocess.run([str(ROOT / 'build/pcemu-hvf'), *args], cwd=ROOT,
                              capture_output=True, text=True, timeout=10)

    def test_raw_guest(self):
        with tempfile.TemporaryDirectory() as tmp:
            kernel = Path(tmp) / 'guest.bin'
            # mov x0,#42; hvc #0
            kernel.write_bytes(struct.pack('<II', 0xd2800540, 0xd4000002))
            result = self.invoke('--machine', 'arm64-test', '--backend', 'hvf',
                                 '--kernel', str(kernel))
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertIn('ec=0x16', result.stdout)
            self.assertIn('x0=0x2a', result.stdout)
            self.assertIn('exits=1', result.stdout)

    def test_uncooperative_guest(self):
        with tempfile.TemporaryDirectory() as tmp:
            kernel = Path(tmp) / 'loop.bin'
            kernel.write_bytes(struct.pack('<I', 0x14000000))  # b .
            result = self.invoke('--kernel', str(kernel), '--timeout-ms', '20')
            self.assertEqual(result.returncode, 1, result.stderr)
            self.assertIn('exit=1 ', result.stdout)  # Canceled

    def test_validation(self):
        cases = [(), ('--kernel', '/missing/hvf-guest.bin'),
                 ('--timeout-ms', '0'), ('--timeout-ms', '60001'),
                 ('--timeout-ms', '20ms'), ('--kernel',),
                 ('--machine', 'win95', '--self-test'),
                 ('--machine', 'win11-arm64', '--self-test'),
                 ('--backend', 'interpreter', '--self-test'),
                 ('--backend', 'kvm', '--self-test'),
                 ('--self-test', '--kernel', 'any'),
                 ('--machine', 'arm64-uefi', '--self-test'),
                 ('--machine', 'arm64-uefi', '--kernel', 'any'),
                 ('--vars', 'any', '--self-test')]
        for args in cases:
            with self.subTest(args=args):
                result = self.invoke(*args)
                self.assertNotEqual(result.returncode, 0)
                self.assertIn('pcemu-hvf:', result.stderr)
        with tempfile.TemporaryDirectory() as tmp:
            kernel = Path(tmp) / 'invalid.bin'
            for contents in (b'', b'abc'):
                kernel.write_bytes(contents)
                result = self.invoke('--kernel', str(kernel))
                self.assertNotEqual(result.returncode, 0)
                self.assertIn('4-byte aligned', result.stderr)

    @unittest.skipUnless((ROOT / 'roms/edk2-arm64-vars.bin').exists(), 'Firmware not downloaded')
    def test_private_variables(self):
        template = ROOT / 'roms/edk2-arm64-vars.bin'
        result = self.invoke('--machine', 'arm64-uefi', '--vars', str(template))
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('private file', result.stderr)
        with tempfile.TemporaryDirectory() as tmp:
            variables = Path(tmp) / 'vars.bin'
            with Path(str(variables) + '.lock').open('wb') as lock:
                fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
                result = self.invoke('--machine', 'arm64-uefi', '--vars', str(variables))
                self.assertNotEqual(result.returncode, 0)
                self.assertIn('locked', result.stderr)
            self.assertFalse(variables.exists())

    @unittest.skipUnless((ROOT / 'roms/edk2-arm64-vars.bin').exists() and
                         (ROOT / 'build/arm64-fw/pcemu-virt.dtb').exists(), 'Firmware assets absent')
    def test_private_disk_overlay(self):
        with tempfile.TemporaryDirectory() as tmp:
            directory = Path(tmp)
            first, second = directory / 'first.raw', directory / 'second.raw'
            first.write_bytes(bytes(8192)); second.write_bytes(bytes(8192))
            variables = directory / 'vars.bin'
            args = ['--machine', 'arm64-uefi', '--vars', str(variables), '--timeout-ms', '1']
            result = self.invoke(*args, '--disk', str(first))
            self.assertEqual(result.returncode, 0, result.stderr)
            overlay = Path(str(variables) + '.pcd')
            self.assertTrue(overlay.read_bytes().startswith(b'PCEMUCOW'))
            self.assertEqual(first.read_bytes(), bytes(8192))
            result = self.invoke(*args, '--disk', str(second))
            self.assertNotEqual(result.returncode, 0)
            self.assertIn('different parent', result.stderr)
            result = self.invoke(*args, '--disk', str(first), '--disk-overlay', str(first))
            self.assertNotEqual(result.returncode, 0)
            self.assertIn('immutable parent', result.stderr)
            with Path(str(overlay) + '.lock').open('wb') as lock:
                fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
                result = self.invoke(*args, '--disk', str(first))
                self.assertNotEqual(result.returncode, 0)
                self.assertIn('already in use', result.stderr)

    def test_help_and_catalog(self):
        for option in ('--help', '--list-machines'):
            result = self.invoke(option)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertIn('arm64-test', result.stdout)


if __name__ == '__main__':
    unittest.main()
