"""Launcher coverage without a GUI or large guest image."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


class Run95Test(unittest.TestCase):
    def test_disk_formats_and_overlay_reuse(self):
        source = Path(__file__).resolve().parents[1] / 'run95.sh'
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            shutil.copy(source, root / 'run95.sh')
            for name in ('media', 'build', 'bin'):
                (root / name).mkdir()
            scripts = {
                'bin/make': '#!/bin/sh\nexit 0\n',
                'build/disk-tool': '#!/bin/sh\necho created >> calls\nprintf PCEMU > "$2"\n',
                'build/pcemu': '#!/bin/sh\nprintf "%s\\n" "$@" > args\n',
            }
            for name, text in scripts.items():
                path = root / name
                path.write_text(text)
                path.chmod(0o755)
            env = dict(os.environ, PATH=str(root / 'bin') + ':' + os.environ['PATH'])
            def run():
                subprocess.run(['bash', str(root / 'run95.sh'), '--headless', '--steps', '1'],
                               env=env, check=True)
                args = (root / 'args').read_text().splitlines()
                self.assertEqual(args[-3:], ['--headless', '--steps', '1'])
                return args[args.index('--disk') + 1]
            base = root / 'media/base.qcow2'
            base.write_bytes(b'QFI\xfb' + bytes(508))
            (root / 'media/win95.raw').symlink_to(base)
            self.assertEqual(run(), 'media/win95-working.pcd')
            overlay = root / 'media/win95-working.pcd'
            overlay.write_bytes(b'saved guest changes')
            self.assertEqual(run(), 'media/win95-working.pcd')
            self.assertEqual(overlay.read_bytes(), b'saved guest changes')
            self.assertEqual((root / 'calls').read_text(), 'created\n')
            self.assertEqual(base.read_bytes()[:4], b'QFI\xfb')
            for signature in (bytes(4), b'PCEM'):
                base.write_bytes(signature + bytes(508))
                self.assertEqual(run(), 'media/win95.raw')


if __name__ == '__main__':
    unittest.main()
