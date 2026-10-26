"""RAM-sized synthetic guest validation; no installed OS or GUI required."""
import json
from pathlib import Path
import subprocess
import tempfile
import time
import unittest

ROOT = Path(__file__).resolve().parents[1]


class SessionControlTests(unittest.TestCase):
    def test_pause_reboot_suspend_restore(self):
        with tempfile.TemporaryDirectory(prefix='pcemu-session-') as tmp:
            work = Path(tmp)
            disk = work / 'boot.raw'
            # Boot sector: increment AX forever, with a valid BIOS signature.
            disk.write_bytes(bytes.fromhex('40 eb fd') + bytes(507) + b'\x55\xaa')
            control, state = work/'control', work/'state.json'
            control.touch()
            log = (work/'session.log').open('w+')
            child = subprocess.Popen([str(ROOT/'build/pcemu'), '--headless', '--no-network',
                '--vgabios', '', '--disk', str(disk), '--steps', '0', '--ram-mb', '2',
                '--control-file', str(control), '--state-json', str(state)], cwd=ROOT,
                stdout=log, stderr=log)

            def send(command):
                with control.open('a') as out:
                    out.write(command+'\n')

            def wait_state():
                deadline = time.monotonic()+10
                while time.monotonic() < deadline:
                    if state.exists():
                        try:
                            return json.loads(state.read_text())
                        except json.JSONDecodeError:
                            pass
                    if child.poll() is not None:
                        log.seek(0)
                        self.fail('Guest exited: '+log.read())
                    time.sleep(.01)
                self.fail('Timed out waiting for capture')

            def capture(command=''):
                state.unlink(missing_ok=True)
                send(command+'\ncapture' if command else 'capture')
                return wait_state()

            try:
                first = capture('pause')
                time.sleep(.05)
                second = capture()
                self.assertEqual(first['instructions'], second['instructions'])
                send('resume')
                time.sleep(.05)
                resumed = capture('pause')
                self.assertGreater(resumed['instructions'], second['instructions'])
                send('reboot')
                time.sleep(.05)
                rebooted = capture('pause')
                self.assertEqual(rebooted['resets'], 1)
                snapshot = work/'saved state'
                send('suspend '+str(snapshot))
                self.assertEqual(child.wait(timeout=10), 0)
                self.assertTrue((snapshot/'state.bin').exists())
                # Invalid save leaves the guest alive and an existing checkpoint intact.
                control.write_text('pause\nsuspend '+str(snapshot)+'\ncapture\n')
                state.unlink(missing_ok=True)
                child = subprocess.Popen([str(ROOT/'build/pcemu'), '--headless', '--no-network',
                    '--load-snapshot', str(snapshot), '--steps', '0', '--paused',
                    '--control-file', str(control), '--state-json', str(state)], cwd=ROOT,
                    stdout=log, stderr=log)
                restored = wait_state()
                self.assertEqual(restored['instructions'], rebooted['instructions'])
                self.assertIsNone(child.poll())
                self.assertTrue((snapshot/'state.bin').exists())
                send('quit')
                self.assertEqual(child.wait(timeout=10), 0)
            finally:
                if child.poll() is None:
                    child.kill(); child.wait()
                log.close()


if __name__ == '__main__':
    unittest.main()
