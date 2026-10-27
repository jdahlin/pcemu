import importlib.util
import math
import sys
import unittest
import tempfile
import json
from datetime import datetime,timezone
import xml.etree.ElementTree as ET
from unittest import mock
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
import analyze_quake_jit as jit_analysis
spec = importlib.util.spec_from_file_location('profile_quake', ROOT / 'tools/profile_quake.py')
profile = importlib.util.module_from_spec(spec)
spec.loader.exec_module(profile)


def baseline(fps):
    return {'executableSha256': 'binary', 'demoSha256': 'demo', 'frames': 50,
            'scoreEligible': True, 'profileMode': 'none', 'windowFps': fps,
            'window': {'threadCpuSeconds': 50 / fps, 'wallSeconds': 50 / fps}}


class ProfilingTests(unittest.TestCase):
    def test_baseline_statistics_and_identity(self):
        runs = [baseline(fps) for fps in [20, 21, 22]]
        result = profile.summarize_baselines(runs)
        self.assertEqual(result['medianFps'], 21)
        self.assertEqual(result['madFps'], 1)
        self.assertAlmostEqual(result['cvPercent'], 100 / 21)
        self.assertEqual(result['threadCpuToWallRatios'], [1, 1, 1])
        runs[-1]['execution'] = dict(dispatch='run')
        with self.assertRaises(ValueError): profile.summarize_baselines(runs)
        runs[-1].pop('execution')
        runs[-1]['executableSha256'] = 'rebuilt'
        with self.assertRaises(ValueError): profile.summarize_baselines(runs)

    def test_instrumented_runs_cannot_be_scores(self):
        for mode in ['host', 'counters']:
            runs = [baseline(20) for _ in range(3)]
            runs[-1]['profileMode'] = mode
            with self.assertRaises(ValueError): profile.summarize_baselines(runs)
        hidden = [baseline(20) for _ in range(3)]
        hidden[0]['window']['guestProfiled'] = True
        with self.assertRaises(ValueError): profile.summarize_baselines(hidden)
        with self.assertRaises(ValueError): profile.summarize_baselines([baseline(20)])
        with self.assertRaises(ValueError): profile.summarize_baselines([baseline(math.nan)] * 3)

    def test_freeze_uses_only_build_inputs_and_detects_concurrent_edits(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder) / 'repo'; root.mkdir()
            (root / 'src').mkdir(); (root / 'src/a.cpp').write_text('original')
            (root / 'platform').mkdir(); (root / 'platform/bridge.cpp').write_text('bridge')
            (root / 'media').mkdir(); (root / 'media/disk.raw').write_bytes(b'guest disk')
            (root / 'CMakeLists.txt').write_text('project(test)')
            names = b'CMakeLists.txt\0src/a.cpp\0platform/bridge.cpp\0media/disk.raw\0'
            with mock.patch.object(profile, 'ROOT', root), mock.patch.object(profile.subprocess, 'check_output', return_value=names):
                destination = Path(folder) / 'frozen'
                manifest = profile.freeze_sources(destination)
                self.assertEqual({r['path'] for r in manifest}, {'CMakeLists.txt', 'src/a.cpp', 'platform/bridge.cpp'})
                self.assertFalse((destination / 'media').exists())
                self.assertEqual((destination / 'src/a.cpp').read_text(), 'original')
                copy = profile.shutil.copy2
                def racing_copy(source, target):
                    copy(source, target)
                    if source.name == 'a.cpp': source.write_text('changed')
                with mock.patch.object(profile.shutil, 'copy2', side_effect=racing_copy), self.assertRaises(RuntimeError):
                    profile.freeze_sources(Path(folder) / 'raced')

    def test_jit_helper_categories_and_timed_address_mapping(self):
        self.assertEqual(jit_analysis.category(['pc::CPU::stepX87(op)', 'pc::Jit::run()']), 'Compiled x87 helpers')
        self.assertEqual(jit_analysis.category(['pc::CPU::execute_string(op)', 'pc::CPU::step()']), 'Interpreter and guest faults')
        with tempfile.TemporaryDirectory() as folder:
            directory=Path(folder);origin=datetime(2026,10,27,tzinfo=timezone.utc);epoch=int(origin.timestamp()*1e9)
            events=[dict(event='header',version=2,clock='unix-ns',pid=42),
                    dict(event='reset',timeNs=epoch,generation=1),
                    dict(event='load',timeNs=epoch+1000000,generation=1,start=4096,end=4104,arm64='0000000000000000',
                         cr3=0,cs=8,ip=16,regions=[dict(offset=0,kind='Alu')])]
            (directory/'jit-debug.jsonl').write_text('\n'.join(json.dumps(e) for e in events))
            frame=ET.fromstring('<frame name="0x1005" addr="0x1005"/>')
            helper=ET.fromstring('<frame name="pc::CPU::stepX87(op)" addr="0x2000"/>')
            rows=[(500000000,'Main Thread (pid: 42)',[frame],{}),
                  (1200000000,'Main Thread (pid: 42)',[frame],{}),
                  (1300000000,'Main Thread (pid: 42)',[helper],{}),
                  (1400000000,'worker (pid: 42)',[helper],{})]
            window=dict(beginUnixNs=epoch+1000000000,endUnixNs=epoch+2000000000)
            with mock.patch.object(jit_analysis,'trace_start',return_value=origin), \
                 mock.patch.object(jit_analysis,'trace_rows',return_value=rows):
                result=jit_analysis.analyze(directory,window,42)
                self.assertEqual(result['mainThreadSamples'],2)
                self.assertEqual(result['outsideWindowSamples'],1)
                self.assertEqual(result['workerSamples'],1)
                self.assertEqual(result['unmappedGeneratedSelfSamples'],0)
                self.assertEqual(sum(row['samples'] for row in result['categories']),2)
                self.assertEqual(result['jitBlocks'][0]['ip'],16)
                with self.assertRaises(ValueError): jit_analysis.analyze(directory,window,43)

    def test_host_tree_conserves_self_samples_and_deduplicates_recursion(self):
        text = '''Call graph:
    10 Thread_1 DispatchQueue_1: com.apple.main-thread (serial)
      10 start (in dyld) + 1
        10 main (in quake-bench) + 1
          8 pc::CPU::stepImpl() (in quake-bench) + 1
            5 pc::CPU::read(unsigned int, int) (in quake-bench) + 1
            3 pc::CPU::stepImpl() (in quake-bench) + 2
          2 nanosleep (in libsystem_kernel.dylib) + 1
    1000 Thread_2 background
      1000 other (in other) + 1
Total number in stack:
'''
        result = profile.analyze_sample(text)
        self.assertEqual(result['mainThreadSamples'], 10)
        self.assertEqual(result['nonIdleSamples'], 8)
        self.assertEqual(result['idleLeafSamples'], 2)
        self.assertEqual(sum(r['samples'] for r in result['self']), 10)
        inclusive = {r['symbol']: r['samples'] for r in result['inclusive']}
        self.assertEqual(inclusive['pc::CPU::stepImpl()'], 8)
        self.assertNotIn('other', inclusive)
        with self.assertRaises(ValueError):
            profile.analyze_sample(text.replace('5 pc::CPU::read', '15 pc::CPU::read'))
        with self.assertRaises(ValueError): profile.analyze_sample('No stack collection occurred')
        with self.assertRaises(ValueError): profile.analyze_sample('Call graph:\nTotal number in stack:')


if __name__ == '__main__':
    unittest.main()
