"""CLI checks; run after make build/pcemu. No installation media required."""
import json
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class MachineProfiles(unittest.TestCase):
    def invoke(self, *args):
        return subprocess.run([str(ROOT / 'build/pcemu'), *args], cwd=ROOT,
                              capture_output=True, text=True, timeout=15)

    def test_catalog(self):
        result = self.invoke('--list-machines')
        self.assertEqual(result.returncode, 0, result.stderr)
        for name in ('pc-legacy', 'win311', 'win95'):
            self.assertIn(name + ' [available]', result.stdout)
        for name in ('win11-arm64', 'win11-x64'):
            self.assertIn(name + ' [planned]', result.stdout)

    def test_configuration(self):
        for profile, ram, extra, backend in (
            ('pc-legacy', 16, [], 'jit'),
            ('win311', 16, ['--backend', 'interpreter'], 'interpreter'),
            ('win95', 32, [], 'jit'),
            ('win95', 8, ['--ram-mb', '8', '--no-jit'], 'interpreter'),
        ):
            with self.subTest(profile=profile, extra=extra), tempfile.TemporaryDirectory() as tmp:
                state = str(Path(tmp) / 'state.json')
                disk = Path(tmp) / 'boot.raw'
                disk.write_bytes(b'\xf4' + bytes(509) + b'\x55\xaa')
                result = self.invoke('--machine', profile, '--headless', '--steps', '1',
                                     '--no-network', '--disk', str(disk), '--vgabios', '',
                                     '--state-json', state, *extra)
                self.assertEqual(result.returncode, 0, result.stderr)
                data = json.loads(Path(state).read_text())
                self.assertEqual(data['machineProfile'], profile)
                self.assertEqual(data['machinePlatform'], 'pc-legacy-v1')
                self.assertEqual(data['ramBytes'], ram * 1024 * 1024)
                self.assertEqual(data['executionBackend'], backend)

    def test_rejects_unimplemented_and_invalid(self):
        cases = [('--machine', 'arm64-test'), ('--machine', 'win11-arm64'), ('--machine', 'win11-x64'),
                 ('--machine', 'unknown'), ('--backend', 'unknown'),
                 ('--backend', 'hvf'), ('--backend', 'kvm'),
                 ('--backend', 'jit', '--no-jit'), ('--ram-mb', '4096')]
        for args in cases:
            with self.subTest(args=args):
                result = self.invoke(*args, '--headless', '--steps', '1')
                self.assertNotEqual(result.returncode, 0)
                self.assertTrue(result.stderr)


if __name__ == '__main__':
    unittest.main()
