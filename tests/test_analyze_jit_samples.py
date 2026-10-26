import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
from analyze_jit_samples import CodeMap, attribute, validate_process


def block(start=4096):
    return {'start': start, 'end': start + 32, 'regions': [
        {'offset': 0, 'kind': 'prologue'},
        {'offset': 8, 'kind': 'Memory', 'memoryPush': True},
        {'offset': 24, 'kind': 'finish'}]}


class JitSampleAttribution(unittest.TestCase):
    def test_process_identity_is_required(self):
        validate_process('Process: pcemu [42]\n', [{'pid': 42}])
        for blocks in ([], [{}], [{'pid': 43}], [{'pid': 42}, {'pid': 43}]):
            with self.assertRaises(ValueError):
                validate_process('Process: pcemu [42]\n', blocks)
        with self.assertRaises(ValueError):
            validate_process('No process header', [{'pid': 42}])

    def test_half_open_ranges_and_gaps(self):
        code = CodeMap([block(), block(8192)])
        self.assertIsNone(code.lookup(4095))
        self.assertEqual(code.lookup(4096)['kind'], 'prologue')
        self.assertEqual(code.lookup(4104)['kind'], 'Memory')
        self.assertEqual(code.lookup(4127)['kind'], 'finish')
        self.assertIsNone(code.lookup(4128))
        self.assertIsNone(code.lookup(8000))
        self.assertEqual(code.lookup(8192)['kind'], 'prologue')

    def test_address_reuse_is_not_silently_guessed(self):
        for second in (4096, 4100):
            with self.assertRaisesRegex(ValueError, 'ambiguous'):
                CodeMap([block(), block(second)])

    def test_skipped_operations_have_no_address_range(self):
        b = block()
        b['regions'].insert(2, {'offset': 8, 'kind': 'Alu'})
        self.assertEqual(CodeMap([b]).lookup(4104)['kind'], 'Alu')

    def test_invalid_regions_are_rejected(self):
        for offsets in ([], [1], [0, 32], [0, 16, 8], [-1, 8]):
            b = block()
            b['regions'] = [{'offset': n, 'kind': 'Alu'} for n in offsets]
            with self.assertRaises(ValueError):
                CodeMap([b])

    def test_exclusive_self_and_helper_counts(self):
        root = ['Thread_1: Main Thread', 'pc::Jit::run()']
        anon = '???  (in <unknown binary>)  [0x100c]'
        nodes = [
            {'self': 4, 'symbol': anon, 'ancestors': root},
            {'self': 3, 'symbol': 'pc::nativeMemory()', 'ancestors': root + [anon]},
            {'self': 2, 'symbol': 'pc::CPU::step()', 'ancestors': root + [anon]},
            {'self': 1, 'symbol': 'other', 'ancestors': root},
            {'self': 9, 'symbol': anon, 'ancestors': root + ['waitForProfiler()']},
            {'self': 9, 'symbol': anon, 'ancestors': ['Thread_2: worker']},
        ]
        result = attribute(nodes, CodeMap([block()]))
        self.assertEqual(result['mainThreadTaskSamples'], 10)
        self.assertEqual(result['mappedSamples'], 9)
        self.assertEqual(result['unmappedSamples'], 1)
        self.assertEqual(sum(r['samples'] for r in result['regions']), 9)
        self.assertEqual({r['scope'] for r in result['regions']},
                         {'generated self', 'helper descendants', 'interpreter replay'})
        self.assertTrue(all(r['region'] == 'Memory/PUSH source and stack'
                            for r in result['regions']))


if __name__ == '__main__':
    unittest.main()
