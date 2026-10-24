"""Regression: capturing before GOP initialization must leave the VM running."""
from pathlib import Path
import os
import platform
import signal
import struct
import subprocess
import tempfile
import time
import unittest

ROOT = Path(__file__).resolve().parents[1]
RUNNER = ROOT / 'build/pcemu-hvf'


@unittest.skipUnless(platform.system() == 'Darwin' and platform.machine() == 'arm64'
                     and RUNNER.exists()
                     and (ROOT / 'build/arm64-fw/pcemu-virt.dtb').exists(),
                     'requires built Apple Silicon HVF runner and firmware assets')
class LiveCapture(unittest.TestCase):
    def test_no_gop_capture_does_not_stop_guest(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            firmware = root / 'loop.bin'
            firmware.write_bytes(struct.pack('<I', 0x14000000))  # b .; never initializes GOP
            variables = root / 'vars.bin'
            log = root / 'host.log'
            with log.open('wb') as stderr:
                process = subprocess.Popen([
                    str(RUNNER), '--machine', 'arm64-uefi', '--firmware', str(firmware),
                    '--vars', str(variables), '--run-forever',
                    '--framebuffer-ppm', str(root / 'capture.ppm')],
                    cwd=ROOT, stdout=subprocess.DEVNULL, stderr=stderr)
                try:
                    deadline = time.monotonic() + 10
                    while not Path(str(variables) + '.lock').exists() and process.poll() is None:
                        if time.monotonic() > deadline:
                            self.fail('VM initialization timed out')
                        time.sleep(0.02)
                    self.assertIsNone(process.poll(), log.read_text())
                    os.kill(process.pid, signal.SIGUSR1)
                    while 'UEFI live capture unavailable:' not in log.read_text():
                        self.assertIsNone(process.poll(), log.read_text())
                        if time.monotonic() > deadline:
                            self.fail('Live capture was not handled')
                        time.sleep(0.02)
                    self.assertIsNone(process.poll(), 'Diagnostic stopped the VM')
                    self.assertIn('No valid GOP framebuffer', log.read_text())
                finally:
                    if process.poll() is None:
                        process.terminate()
                    process.wait(timeout=5)


if __name__ == '__main__':
    unittest.main()
