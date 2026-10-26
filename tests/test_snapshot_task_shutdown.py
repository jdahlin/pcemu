"""Exercise benchmark shutdown acceptance without VM media or disk overlays."""
import base64
import json
from pathlib import Path
import subprocess
import tempfile
import time
import unittest

ROOT = Path(__file__).resolve().parents[1]
FAKE = r'''#!/usr/bin/env python3
import json,sys,time
from pathlib import Path
args=sys.argv[1:]
def arg(name): return Path(args[args.index(name)+1])
control,frame,state=map(arg,['--control-file','--frame','--state-json'])
done=False
position=0
while True:
    lines=control.read_text().splitlines()
    for command in lines[position:]:
        if command=='complete': done=True
        if command=='capture':
            frame.write_bytes(b'P6\n1 1\n255\n'+(b'\xff\xff\xff' if done else b'\x00\x00\x00'))
            state.write_text(json.dumps(dict(executionEpoch=1,instructions=int(done),nativeInstructions=int(done),compiledBlocks=0,disk='')))
        if command=='quit':
            time.sleep(.15)
            control.with_name("exit-ready").touch()
            sys.exit(EXIT_STATUS)
    position=len(lines)
    time.sleep(.002)
'''


class SnapshotShutdownTests(unittest.TestCase):
    def run_task(self, directory, status=0, extra=()):
        engine = directory/'emulator'
        engine.write_text(FAKE.replace('EXIT_STATUS', str(status)))
        engine.chmod(0o755)
        task = directory/'task.json'
        task.write_text(json.dumps(dict(name='shutdown fixture',rectangle=[0,0,1,1],
            before=base64.b64encode(bytes(3)).decode(),
            after=base64.b64encode(bytes([255]*3)).decode(),commands=['complete'])))
        return subprocess.run(['python3',str(ROOT/'tools/bench_snapshot_task.py'),
            str(directory/'snapshot'),'--task',str(task),'--emulator',str(engine),
            '--repeat','1','--output',str(directory/'output'),*extra],
            capture_output=True,text=True,timeout=10)

    def test_delayed_clean_exit_is_reported_separately(self):
        with tempfile.TemporaryDirectory() as tmp:
            directory=Path(tmp)
            result=self.run_task(directory,extra=['--shutdown-timeout','2'])
            self.assertEqual(result.returncode,0,result.stdout+result.stderr)
            report=json.loads((directory/'output/result.json').read_text())
            self.assertEqual(report['shutdownTimeoutSeconds'],2)
            self.assertEqual(report['samples'][0]['exitStatus'],0)
            self.assertGreaterEqual(report['samples'][0]['shutdownWaitMs'],100)
            self.assertFalse((directory/'output/run-0/frame.ppm').exists())

    def test_nonzero_exit_rejects_successful_pixels_and_retains_files(self):
        with tempfile.TemporaryDirectory() as tmp:
            directory=Path(tmp)
            result=self.run_task(directory,status=7)
            self.assertNotEqual(result.returncode,0)
            self.assertIn('shutdown failed with status 7',result.stderr)
            self.assertFalse((directory/'output/result.json').exists())
            self.assertTrue((directory/'output/run-0/frame.ppm').exists())

    def test_shutdown_timeout_preserves_guest_files(self):
        with tempfile.TemporaryDirectory() as tmp:
            directory=Path(tmp)
            result=self.run_task(directory,extra=['--shutdown-timeout','0.01'])
            # The owned fixture finishes by itself; do not leave a test process alive.
            marker=directory/'output/run-0/exit-ready'
            deadline=time.monotonic()+2
            while not marker.exists() and time.monotonic()<deadline:
                time.sleep(.01)
            self.assertTrue(marker.exists())
            self.assertNotEqual(result.returncode,0)
            self.assertIn('TimeoutExpired',result.stderr)
            self.assertIn('left alive',result.stdout)
            self.assertFalse((directory/'output/result.json').exists())
            self.assertTrue((directory/'output/run-0/frame.ppm').exists())

    def test_invalid_shutdown_timeout_rejected_before_launch(self):
        for value in ['0','-1','nan','inf','3601']:
            with self.subTest(value=value), tempfile.TemporaryDirectory() as tmp:
                directory=Path(tmp)
                result=self.run_task(directory,extra=['--shutdown-timeout',value])
                self.assertEqual(result.returncode,2,result.stderr)
                self.assertIn('--shutdown-timeout must be positive',result.stderr)
                self.assertFalse((directory/'output').exists())


if __name__ == '__main__':
    unittest.main()
