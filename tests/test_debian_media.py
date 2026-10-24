"""Check the installer ESP layout and immutable-output guard without downloads."""
from pathlib import Path
import sys
import tempfile
import unittest
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
from prepare_debian_arm64 import create, CMDLINE
from prepare_media import FAT


class DebianMedia(unittest.TestCase):
    def test_payload_readback(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            kernel = bytes(range(256)) * 8193
            initrd = b'initrd' * 10001
            (root / 'linux').write_bytes(kernel)
            (root / 'initrd.gz').write_bytes(initrd)
            path = root / 'netboot.raw'
            create(path, root)
            self.assertEqual(path.stat().st_size, 256 * 1024 * 1024)
            fat = FAT(path)
            try:
                self.assertEqual(fat.bits, 16)
                files = {name: fat.read_file(cluster, size) for _, name, cluster, size in fat.entries()}
                self.assertEqual(files, {'LINUX   EFI': kernel, 'INITRD  GZ ': initrd,
                    'STARTUP NSH': ('fs0:\\LINUX.EFI ' + CMDLINE + '\r\n').encode()})
            finally:
                fat.f.close()
            with self.assertRaises(FileExistsError):
                create(path, root)


if __name__ == '__main__':
    unittest.main()
