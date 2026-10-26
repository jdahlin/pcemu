"""Integration checks for the signed Apple Silicon HVF runner."""
import fcntl
from pathlib import Path
import struct
import subprocess
import sys
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
        cases = [('--virtio-balloon', '--self-test'), ('--balloon-target-mb', '16385'), ('--balloon-target-mb', 'oops'), ('--balloon-pfn64', '--self-test'), ('--virtio-serial', '--self-test'), ('--guest-agent-socket', 'any', '--self-test'), ('--gui', '--self-test'), ('--hyperv-probe-ipi',), ('--hyperv', 'bogus'), ('--hyperv', 'basic', '--self-test'), ('--hyperv', 'synic', '--self-test'), ('--hyperv', 'vmbus', '--self-test'),
                 ('--hyperv-stats', 'any', '--self-test'), (), ('--kernel', '/missing/hvf-guest.bin'),
                 ('--timeout-ms', '0'), ('--timeout-ms', '60001'),
                 ('--timeout-ms', '20ms'), ('--kernel',),
                 ('--machine', 'win95', '--self-test'),
                 ('--machine', 'win11-arm64', '--self-test'),
                 ('--backend', 'interpreter', '--self-test'),
                 ('--backend', 'kvm', '--self-test'),
                 ('--self-test', '--kernel', 'any'),
                 ('--machine', 'arm64-uefi', '--self-test'),
                 ('--machine', 'arm64-uefi', '--kernel', 'any'),
                 ('--vars', 'any', '--self-test'),
                 ('--cpus', '0'), ('--cpus', '9'), ('--cpus', '2oops'),
                 ('--cpus', '2', '--self-test'), ('--network', 'broken'), ('--ssh-port', '65536'),
                 ('--ssh-port', '22oops'), ('--run-forever', '--self-test')]
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
            result = self.invoke(*args, '--disk', str(first), '--cpu-state')
            self.assertIn('esr_el1=0x', result.stderr)
            self.assertIn('elr_el1=0x', result.stderr)
            self.assertEqual(result.returncode, 0, result.stderr)
            overlay = Path(str(variables) + '.qcow2')
            self.assertTrue(overlay.read_bytes().startswith(b'QFI\xfb'))
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

    @unittest.skipUnless((ROOT / 'roms/edk2-arm64-vars.bin').exists(), 'Firmware not downloaded')
    def test_extra_disk_isolated_and_locked(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            base = root / 'blank.raw'; base.write_bytes(bytes(8192))
            variables = root / 'vars.bin'
            args = ['--machine', 'arm64-uefi', '--vars', str(variables), '--timeout-ms', '1']
            result = self.invoke(*args, '--disk', str(base), '--extra-disk', str(base))
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(base.read_bytes(), bytes(8192))
            self.assertTrue(Path(str(variables) + '.qcow2').exists())
            overlay = Path(str(variables) + '.extra.qcow2')
            self.assertTrue(overlay.exists())
            result = self.invoke(*args, '--extra-disk', str(base), '--extra-disk-overlay', str(base))
            self.assertIn('immutable parent', result.stderr)
            with Path(str(overlay) + '.lock').open('wb') as lock:
                fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
                result = self.invoke(*args, '--extra-disk', str(base))
                self.assertIn('already in use', result.stderr)

    @unittest.skipUnless((ROOT / 'roms/edk2-arm64-vars.bin').exists(), 'Firmware not downloaded')
    def test_cpu_table_mismatch(self):
        with tempfile.TemporaryDirectory() as tmp:
            result = self.invoke('--machine', 'arm64-uefi', '--cpus', '2',
                                 '--acpi-dir', 'build/arm64-fw/acpi',
                                 '--vars', str(Path(tmp) / 'vars.bin'), '--timeout-ms', '1')
            self.assertNotEqual(result.returncode, 0)
            self.assertIn('ACPI CPU count does not match', result.stderr)

    @unittest.skipUnless((ROOT / 'roms/edk2-arm64-vars.bin').exists(), 'Firmware not downloaded')
    def test_forever_sigterm_flushes(self):
        import signal
        import time
        with tempfile.TemporaryDirectory() as tmp:
            variables = Path(tmp) / 'vars.bin'
            process = subprocess.Popen([str(ROOT / 'build/pcemu-hvf'), '--machine', 'arm64-uefi',
                '--run-forever', '--timeout-ms', '1', '--vars', str(variables)],
                cwd=ROOT, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
            try:
                time.sleep(0.2)
                self.assertIsNone(process.poll())
                process.send_signal(signal.SIGTERM)
                _, errors = process.communicate(timeout=5)
                self.assertEqual(process.returncode, 0, errors)
                self.assertEqual(variables.stat().st_size, 768 * 1024)
            finally:
                if process.poll() is None:
                    process.kill(); process.communicate()

    @unittest.skipUnless((ROOT / 'roms/edk2-arm64-vars.bin').exists(), 'Firmware not downloaded')
    def test_vmbus_requires_matching_acpi(self):
        sys.path.insert(0, str(ROOT / 'tools'))
        from build_arm_acpi import build
        for bus_tables, mode in ((False, 'vmbus'), (True, 'synic'), (True, 'vmbus')):
            with self.subTest(bus_tables=bus_tables, mode=mode), tempfile.TemporaryDirectory() as tmp:
                root = Path(tmp)
                tables, rsdp, loader, _ = build(hyperv=True, vmbus=bus_tables)
                for name, contents in (('tables', tables), ('rsdp', rsdp), ('table-loader', loader)):
                    (root / name).write_bytes(contents)
                result = self.invoke('--machine', 'arm64-uefi', '--hyperv', mode,
                    '--acpi-dir', str(root), '--dtb', 'build/arm64-fw/pcemu-virt.dtb',
                    '--vars', str(root / 'vars.bin'), '--timeout-ms', '1')
                if mode == 'vmbus' and bus_tables:
                    self.assertEqual(result.returncode, 0, result.stderr)
                else:
                    self.assertNotEqual(result.returncode, 0)
                    self.assertIn('ACPI VMBus does not match', result.stderr)

    @unittest.skipUnless((ROOT / 'roms/edk2-arm64-vars.bin').exists() and
                         (ROOT / 'build/arm64-fw/pcemu-virt.dtb').exists(), 'Firmware assets absent')
    def test_vmbus_default_asset_directory(self):
        sys.path.insert(0, str(ROOT / 'tools'))
        from build_arm_acpi import build
        import shutil
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            (root / 'roms').symlink_to(ROOT / 'roms', target_is_directory=True)
            platform = root / 'build/arm64-fw/cpus1-hyperv-vmbus'
            acpi = platform / 'acpi'
            acpi.mkdir(parents=True)
            tables, rsdp, loader, _ = build(hyperv=True, vmbus=True)
            for name, contents in (('tables', tables), ('rsdp', rsdp), ('table-loader', loader)):
                (acpi / name).write_bytes(contents)
            shutil.copyfile(ROOT / 'build/arm64-fw/pcemu-virt.dtb', platform / 'pcemu-virt.dtb')
            result = subprocess.run([str(ROOT / 'build/pcemu-hvf'), '--machine', 'arm64-uefi',
                '--hyperv', 'vmbus', '--vars', str(root / 'vars.bin'), '--timeout-ms', '1'],
                cwd=root, capture_output=True, text=True, timeout=10)
            self.assertEqual(result.returncode, 0, result.stderr)

    def test_kvp_probe_requires_vmbus(self):
        result = self.invoke('--machine', 'arm64-uefi', '--hyperv', 'basic', '--hyperv-kvp-probe')
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('--hyperv-kvp-probe requires --hyperv vmbus', result.stderr)
        self.assertIn('--hyperv-kvp-probe', self.invoke('--help').stdout)

    def test_kvp_roundtrip_requires_explicit_exclusive_vmbus_mode(self):
        result = self.invoke('--machine', 'arm64-uefi', '--hyperv', 'basic', '--hyperv-kvp-roundtrip')
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('--hyperv-kvp-roundtrip requires VMBus', result.stderr)
        result = self.invoke('--machine', 'arm64-uefi', '--hyperv', 'vmbus', '--hyperv-kvp-roundtrip', '--hyperv-kvp-probe')
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('excludes --hyperv-kvp-probe', result.stderr)

    def test_help_and_catalog(self):
        for option in ('--help', '--list-machines'):
            result = self.invoke(option)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertIn('arm64-test', result.stdout)


if __name__ == '__main__':
    unittest.main()
