"""Exercise CPU accounting against a real sleeping/busy child, not wall time."""
from pathlib import Path
import subprocess
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
from host_process_cpu import CpuSnapshot, cpu_interval, read_process_cpu


class ProcessCpuTest(unittest.TestCase):
    @unittest.skipUnless(sys.platform == 'darwin', 'macOS process accounting')
    def test_live_child_cpu_units_and_sleep_exclusion(self):
        code = '''import sys,time
print('ready',flush=True)
sys.stdin.readline()
time.sleep(.12)
print('slept',flush=True)
sys.stdin.readline()
before=time.process_time_ns()
while time.process_time_ns()-before < 80000000: pass
print('worked',flush=True)
sys.stdin.readline()
'''
        child = subprocess.Popen([sys.executable, '-u', '-c', code],
                                 stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True)
        try:
            self.assertEqual(child.stdout.readline().strip(), 'ready')
            before = read_process_cpu(child.pid)
            child.stdin.write('\n'); child.stdin.flush()
            self.assertEqual(child.stdout.readline().strip(), 'slept')
            slept = read_process_cpu(child.pid)
            child.stdin.write('\n'); child.stdin.flush()
            self.assertEqual(child.stdout.readline().strip(), 'worked')
            worked = read_process_cpu(child.pid)
            idle = cpu_interval(before, slept)
            busy = cpu_interval(slept, worked)
            self.assertGreater(before.start_abstime, 0)
            # The child deliberately consumes 80 ms of CPU even on a busy host.
            self.assertGreater(busy['totalMs'], 70)
            self.assertLess(busy['totalMs'], 200)
            self.assertLess(idle['totalMs'], 50)
            self.assertAlmostEqual(busy['totalMs'], busy['userMs'] + busy['systemMs'])
        finally:
            child.stdin.close()
            child.wait(timeout=10)
            child.stdout.close()

    def test_process_identity_and_counter_regression_are_rejected(self):
        before = CpuSnapshot(12, 123, 100, 200)
        for after in [CpuSnapshot(13, 123, 110, 210), CpuSnapshot(12, 124, 110, 210),
                      CpuSnapshot(12, 123, 99, 210), CpuSnapshot(12, 123, 110, 199)]:
            with self.assertRaises(ValueError):
                cpu_interval(before, after)

    @unittest.skipUnless(sys.platform == 'darwin', 'macOS process accounting')
    def test_missing_process_is_not_reported_as_zero(self):
        with self.assertRaises(OSError):
            read_process_cpu(2147483647)
        with self.assertRaises(ValueError):
            read_process_cpu(0)


if __name__ == '__main__':
    unittest.main()
