"""Verify ARM64 bundle integrity and preservation of the working boot driver."""
import hashlib
import json
from pathlib import Path
import struct
import sys
import tempfile
import unittest
import zipfile
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
from build_windows_driver_bundle import ALL_PACKAGES, build_bundle


class DriverBundle(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.drivers = self.root / 'drivers'
        self.old = self.root / 'old'
        self.output = self.root / 'drivers.zip'
        for package in ALL_PACKAGES:
            path = self.drivers / package
            path.mkdir(parents=True)
            (path / (package + '.inf')).write_text('[Models.NTARM64]\nDriverVer=100.103.104.30200\n')
            (path / (package + '.sys')).write_bytes(self.pe(0xaa64))
        self.old.mkdir()
        (self.old / 'viostor.inf').write_text('[Models.NTARM64]\nDriverVer=100.101.104.28500\n')
        (self.old / 'viostor.sys').write_bytes(self.pe(0xaa64) + b'known-working')
        (self.drivers / 'virtio-win_license.txt').write_text('fixture license')

    def pe(self, machine):
        data = bytearray(128)
        data[:2] = b'MZ'
        struct.pack_into('<I', data, 60, 96)
        data[96:100] = b'PE\0\0'
        struct.pack_into('<H', data, 100, machine)
        return bytes(data)

    def test_preserves_boot_driver_and_hashes_every_payload(self):
        manifest = build_bundle(self.drivers, self.old, self.output)
        with zipfile.ZipFile(self.output) as archive:
            self.assertEqual(json.loads(archive.read('manifest.json')), manifest)
            self.assertEqual(archive.read('viostor/viostor.sys'), (self.old / 'viostor.sys').read_bytes())
            for file in manifest['files']:
                self.assertEqual(hashlib.sha256(archive.read(file['path'])).hexdigest(), file['sha256'])
            self.assertEqual(len([n for n in archive.namelist() if n.endswith('.inf')]), 12)
        with self.assertRaises(FileExistsError):
            build_bundle(self.drivers, self.old, self.output)

    def test_rejects_x64_kernel_payload(self):
        (self.drivers / 'NetKVM/NetKVM.sys').write_bytes(self.pe(0x8664))
        with self.assertRaisesRegex(ValueError, 'Non-ARM64'):
            build_bundle(self.drivers, self.old, self.output)
        self.assertFalse(self.output.exists())

    def test_rejects_boot_driver_regression(self):
        (self.old / 'viostor.inf').write_text('[Models.NTARM64]\nDriverVer=100.103.104.30200\n')
        with self.assertRaisesRegex(ValueError, 'INTx-only'):
            build_bundle(self.drivers, self.old, self.output)


if __name__ == '__main__':
    unittest.main()
