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
            for name in ('media', 'build', 'bin', 'tools'):
                (root / name).mkdir()
            scripts = {
                'tools/build.sh': '#!/bin/sh\nexit 0\n',
                'build/disk-tool': '#!/bin/sh\necho created >> calls\nprintf PCEMU > "$2"\n',
                'build/pcemu': '#!/bin/sh\nprintf "%s\\n" "$@" > args\n',
            }
            for name, text in scripts.items():
                path = root / name
                path.write_text(text)
                path.chmod(0o755)
            env = dict(os.environ, PATH=str(root / 'bin') + ':' + os.environ['PATH'])
            def run(*options):
                subprocess.run(['bash', str(root / 'run95.sh'), *options, '--headless', '--steps', '1'],
                               env=env, check=True)
                args = (root / 'args').read_text().splitlines()
                self.assertEqual(args[-3:], ['--headless', '--steps', '1'])
                return args[args.index('--disk') + 1]
            base = root / 'media/base.qcow2'
            base.write_bytes(b'QFI\xfb' + bytes(508))
            (root / 'media/win95.raw').symlink_to(base)
            self.assertEqual(run(), 'media/win95-working.qcow2')
            overlay = root / 'media/win95-working.qcow2'
            overlay.write_bytes(b'saved guest changes')
            self.assertEqual(run(), 'media/win95-working.qcow2')
            self.assertEqual(overlay.read_bytes(), b'saved guest changes')
            self.assertEqual((root / 'calls').read_text(), 'created\n')
            self.assertEqual(base.read_bytes()[:4], b'QFI\xfb')
            for signature in (bytes(4), b'PCEM'):
                base.write_bytes(signature + bytes(508))
                self.assertEqual(run(), 'media/win95.raw')
            svga = root / 'media/win95-svga.qcow2'
            svga.write_bytes(b'configured SVGA disk')
            self.assertEqual(run(), 'media/win95-svga.qcow2')
            self.assertEqual(run('--vga'), 'media/win95.raw')
            self.assertNotIn('--vga', (root / 'args').read_text().splitlines())
            base.write_bytes(b'QFI\xfb' + bytes(508))
            self.assertEqual(run('--vga'), 'media/win95-working.qcow2')
            self.assertEqual(run(), 'media/win95-svga.qcow2')
            self.assertEqual(svga.read_bytes(), b'configured SVGA disk')
            monitor = root / 'media/win95-monitor.qcow2'
            monitor.write_bytes(b'configured monitor and widescreen modes')
            self.assertEqual(run(), 'media/win95-monitor.qcow2')
            self.assertEqual(run('--vga'), 'media/win95-working.qcow2')
            self.assertEqual(svga.read_bytes(), b'configured SVGA disk')
            self.assertEqual(monitor.read_bytes(), b'configured monitor and widescreen modes')
            seamless = root / 'media/win95-mouse.qcow2'
            seamless.write_bytes(b'configured seamless mouse')
            self.assertEqual(run(), 'media/win95-mouse.qcow2')
            self.assertEqual(run('--vga'), 'media/win95-working.qcow2')
            self.assertEqual(seamless.read_bytes(), b'configured seamless mouse')
            native = root / 'media/win95-native.qcow2'
            native.write_bytes(b'QFI\xfb'+b'configured native cursor')
            self.assertEqual(run(), 'media/win95-native.qcow2')
            self.assertEqual(run('--vga'), 'media/win95-working.qcow2')


if __name__ == '__main__':
    unittest.main()
