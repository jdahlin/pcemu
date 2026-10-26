"""Tiny synthetic media verifies raw/COW ISO staging without installation fixtures."""
import hashlib
from pathlib import Path
import struct
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]

class ISOStaging(unittest.TestCase):
    def test_raw_and_compressed_preserve_parent_and_verify_payload(self):
        with tempfile.TemporaryDirectory() as tmp:
            work = Path(tmp)
            parent = work / 'parent.raw'
            boot = bytearray(512)
            boot[3:11] = b'MSDOS5.0'
            struct.pack_into('<HBHBHHBH', boot, 11, 512, 1, 1, 2, 32, 5000, 0xf8, 20)
            boot[510:] = b'\x55\xaa'
            with parent.open('wb') as f:
                f.write(boot)
                for sector in (1, 21):
                    f.seek(sector * 512)
                    f.write(b'\xf8\xff\xff\xff')
                f.truncate(5000 * 512)
            original = hashlib.sha256(parent.read_bytes()).hexdigest()
            def record(name, extent, size, directory=False):
                data = bytearray(33 + len(name) + (len(name) % 2 == 0))
                data[0] = len(data)
                struct.pack_into('<I', data, 2, extent)
                struct.pack_into('<I', data, 10, size)
                data[25] = 2 if directory else 0
                data[32] = len(name)
                data[33:33+len(name)] = name
                return data
            payload = bytes(range(256)) * 19
            image = bytearray(24 * 2048)
            image[16*2048:16*2048+7] = b'\x01CD001\x01'
            image[16*2048+156:16*2048+190] = record(b'\0', 20, 2048, True)
            entries = record(b'\0', 20, 2048, True) + record(b'\1', 20, 2048, True) + record(b'SETUP.EXE;1', 21, len(payload))
            image[20*2048:20*2048+len(entries)] = entries
            image[21*2048:21*2048+len(payload)] = payload
            iso = work / 'tiny.iso'
            iso.write_bytes(image)
            for compressed in (False, True):
                disk = work / ('child.qcow2' if compressed else 'copy.raw')
                if compressed:
                    subprocess.run([str(ROOT/'build/disk-tool'), 'overlay', str(disk), str(parent)], check=True)
                else:
                    disk.write_bytes(parent.read_bytes())
                command = ['python3', str(ROOT/'tools/stage_iso.py'), str(iso), str(disk)]
                for extra in ([], ['--verify-only']):
                    result = subprocess.run(command + extra, capture_output=True, text=True)
                    self.assertEqual(result.returncode, 0, result.stderr)
                    self.assertIn('Verified 1 files', result.stdout)
                duplicate = subprocess.run(command, capture_output=True, text=True)
                self.assertNotEqual(duplicate.returncode, 0)
                self.assertIn('already exists', duplicate.stderr)
            self.assertEqual(hashlib.sha256(parent.read_bytes()).hexdigest(), original)

if __name__ == '__main__':
    unittest.main()
