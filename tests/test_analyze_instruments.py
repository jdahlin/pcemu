"""Check Instruments reference resolution and strict guest-task window accounting."""
from datetime import datetime, timezone
import sys
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
import analyze_instruments


class InstrumentsAccounting(unittest.TestCase):
    def test_only_task_samples_count_and_anonymous_jit_is_execution(self):
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            run = directory / 'run-0'
            run.mkdir()
            (run / 'time-profile.xml').write_text('''<trace-query-result><node>
              <row><sample-time id="t0">500000000</sample-time>
                <thread id="main" fmt="Main Thread (pcemu)"/>
                <tagged-backtrace id="jit"><backtrace>
                  <frame id="anonymous" name="0x10a4f32b5"/>
                  <frame id="entry" name="pc::Jit::run()"/>
                </backtrace></tagged-backtrace></row>
              <row><sample-time id="t1">1500000000</sample-time>
                <thread ref="main"/><tagged-backtrace ref="jit"/></row>
              <row><sample-time id="t2">1800000000</sample-time>
                <thread ref="main"/><tagged-backtrace><backtrace>
                  <frame id="step" name="pc::CPU::stepImpl()">
                    <source line="721"><path id="path">/tmp/src/cpu.cpp</path></source>
                  </frame><frame name="pc::CPU::step()"/>
                </backtrace></tagged-backtrace></row>
              <row><sample-time id="t3">2500000000</sample-time>
                <thread ref="main"/><tagged-backtrace ref="jit"/></row>
              <row><sample-time id="t4">1800000000</sample-time>
                <thread id="worker" fmt="Worker (pcemu)"/>
                <tagged-backtrace ref="jit"/></row>
              <row><sample-time id="t5">1800000000</sample-time>
                <thread ref="main"/><sentinel/></row>
            </node></trace-query-result>''')
            report = {'samples': [{'taskStartUtc': '2026-10-26T12:00:01+00:00',
                                   'taskEndUtc': '2026-10-26T12:00:02+00:00'}]}
            origin = datetime(2026, 10, 26, 12, tzinfo=timezone.utc)
            with patch.object(analyze_instruments, 'trace_start', return_value=origin):
                result = analyze_instruments.analyze(report, directory)
            self.assertEqual(result['samples'], 2)
            self.assertEqual(result['categories'][0]['name'],
                             'Generated code and native helpers')
            self.assertEqual(result['categories'][1]['name'],
                             'Interpreter (including MMU and guest faults)')
            self.assertEqual(result['outsideTaskSamples'], 2)
            self.assertEqual(sum(result['workerSamples'].values()), 1)
            self.assertEqual([part['samples'] for part in result['timeQuartiles']],
                             [0, 0, 1, 1])
            self.assertEqual(result['selfSites'][1]['name'],
                             'pc::CPU::stepImpl() [cpu.cpp:721]')


if __name__ == '__main__':
    unittest.main()
