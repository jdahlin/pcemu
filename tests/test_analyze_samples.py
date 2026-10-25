"""Host-profile accounting must survive native-helper namespace changes."""
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
from analyze_samples import category, parse, source_site


class HostSampleAccounting(unittest.TestCase):
    def test_native_helpers_keep_their_execution_category(self):
        for helper in ('pc::nativeMemory()', 'pc::jit_detail::nativeMemory()',
                       'pc::jit_detail::nativeFinish()'):
            node = {'ancestors': ['pc::Jit::run()', helper], 'symbol': 'pc::CPU::physical()'}
            self.assertEqual(category(node), 'Generated code and native helpers')

    def test_replay_and_disk_costs_are_not_dispatch(self):
        self.assertEqual(category({'ancestors': ['pc::Jit::run()', 'pc::CPU::step()'],
                                   'symbol': 'pc::CPU::physical()'}),
                         'Interpreter (including MMU and guest faults)')
        self.assertEqual(category({'ancestors': ['pc::Jit::run()',
                                                'pc::jit_detail::nativeMemory()'],
                                   'symbol': 'pc::Disk::read()'}), 'Disk and compression')

    def test_attachment_wait_is_excluded(self):
        self.assertEqual(category({'ancestors': ['main', 'waitForProfiler(unsigned int)'],
                                   'symbol': 'nanosleep'}),
                         'Profiler attachment barrier (excluded)')

    def test_frame_renderer_is_not_other_host_work(self):
        for function in ('pc::VGA::render(', 'pc::PcDisplay::save(', 'pc::PcDisplay::render('):
            self.assertEqual(category({'ancestors': [function], 'symbol': 'memcpy'}),
                             'Capture/video/control output')
        self.assertEqual(category({'ancestors': ['main'], 'symbol': '???  (in <unknown binary>)'}),
                         'Unresolved code without JIT ancestry')

    def test_source_sites_ignore_aslr_and_keep_line(self):
        a = 'pc::Jit::run()  (in pcemu) + 432,548,...  [0x1000,0x2000,...]  jit.cpp:89'
        b = 'pc::Jit::run()  (in pcemu) + 432,548,...  [0x3000,0x4000,...]  jit.cpp:89'
        self.assertEqual(source_site(a), 'pc::Jit::run() [jit.cpp:89]')
        self.assertEqual(source_site(a), source_site(b))
        self.assertNotEqual(source_site(a), source_site(a.replace(':89', ':90')))

    def test_inclusive_frames_do_not_duplicate_samples(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'sample.txt'
            path.write_text('Call graph:\n'
                            '    10 Thread_1 Main Thread\n'
                            '    + 10 main\n'
                            '    +   6 pc::Jit::run()\n'
                            '    +   ! 4 pc::jit_detail::nativeMemory()\n'
                            '    +   ! + 4 pc::CPU::physical()\n'
                            '    +   4 waitForProfiler(unsigned int)\n'
                            'Total number in stack\n')
            nodes = parse(path)
            self.assertEqual(sum(node['self'] for node in nodes), 10)
            groups = {}
            for node in nodes:
                groups[category(node)] = groups.get(category(node), 0) + node['self']
            self.assertEqual(groups['Generated code and native helpers'], 4)
            self.assertEqual(groups['JIT dispatch/cache/validation'], 2)
            self.assertEqual(groups['Profiler attachment barrier (excluded)'], 4)


if __name__ == '__main__':
    unittest.main()
