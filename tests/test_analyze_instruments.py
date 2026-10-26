"""Check Instruments reference resolution and strict guest-task window accounting."""
from datetime import datetime, timezone
import sys
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch
import json
import xml.etree.ElementTree as ET

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
import analyze_instruments


class InstrumentsAccounting(unittest.TestCase):
    def test_tagged_pcs_helper_returns_and_process_identity(self):
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            run = directory/'run-0'
            run.mkdir()
            origin = datetime(2026, 10, 26, 12, tzinfo=timezone.utc)
            epoch = int(origin.timestamp()*1e9)
            events = [dict(event='header', version=1, clock='unix-ns', pid=42),
                      dict(event='reset', timeNs=epoch, generation=1),
                      dict(event='load', timeNs=epoch+100_000_000, generation=1,
                           start=4096, end=4104, arm64='0000000000000000', cr3=0, cs=8, ip=16,
                           regions=[dict(offset=0, kind='Prologue'), dict(offset=4, kind='Alu')])]
            (run/'jit-debug.jsonl').write_text('\n'.join(json.dumps(e) for e in events))
            frames = [ET.fromstring('<frame name="0x1005" addr="0x1005"/>')]
            helper = [ET.fromstring('<frame name="native_helper" addr="0x2000"/>'),
                      ET.fromstring('<frame name="0x1009" addr="0x1009"/>')]
            rows = [(1_200_000_000, 'Main Thread (pcemu, pid: 42)', frames, {}),
                    (1_300_000_000, 'Main Thread (pcemu, pid: 42)', helper, {})]
            report = dict(jitDebugMapped=True, samples=[
                dict(taskStartUtc='2026-10-26T12:00:01+00:00',
                     taskEndUtc='2026-10-26T12:00:02+00:00')])
            with patch.object(analyze_instruments, 'trace_start', return_value=origin), \
                 patch.object(analyze_instruments, 'trace_rows', return_value=rows):
                result = analyze_instruments.analyze(report, directory)
                self.assertEqual([r['region'] for r in result['jitRegions']], ['Alu', 'Alu'])
                self.assertEqual(result['unmappedJitSelfSamples'], 0)
                self.assertEqual(result['jitBlocks'][0]['samples'], 2)
            rows[0] = (rows[0][0], 'Main Thread (pcemu, pid: 43)', frames, {})
            with patch.object(analyze_instruments, 'trace_start', return_value=origin), \
                 patch.object(analyze_instruments, 'trace_rows', return_value=rows), \
                 self.assertRaisesRegex(ValueError, 'PID differ'):
                analyze_instruments.analyze(report, directory)

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
