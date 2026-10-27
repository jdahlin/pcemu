import json
from pathlib import Path
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
from analyze_guest_profiles import analyze


def op(code, attempts, leaf=0, samples=0, raw=0, adjusted=0, faults=0, exceptions=0):
    return dict(opcode=code, attempts=attempts, registerOperands=0, memoryOperands=0,
                groups=[0] * 8, cpuLeafAttempts=leaf, cpuSamples=samples, cpuRawNs=raw,
                cpuAdjustedNs=adjusted, cpuFaultSamples=faults, cpuExceptionSamples=exceptions)


def profile(rows, timing=True, **unknown):
    attempts = sum(r['attempts'] for r in rows if r['opcode'] < 256)
    result = dict(startInstructions=0, stopInstructions=attempts, interpretedAttempts=attempts,
                  events=dict(dispatch=attempts, negativeHit=0), opcodes=rows)
    if timing:
        result['cpuTiming'] = dict(version=1, clock='thread-cpu-ns', available=True,
                                  unknownAttempts=0, unknownSamples=0, unknownRawNs=0, unknownAdjustedNs=0)
        result['cpuTiming'].update(unknown)
    return result


class GuestCpuAccounting(unittest.TestCase):
    def read(self, *profiles):
        with tempfile.TemporaryDirectory() as directory:
            paths = []
            for n, value in enumerate(profiles):
                path = Path(directory) / f'{n}.json'
                path.write_text(json.dumps(value))
                paths.append(path)
            return analyze(paths)

    def test_extended_leaf_cpu_is_not_double_counted(self):
        result = self.read(profile([op(15, 4), op(0x1a3, 4, 4, 1, 30, 20), op(0x90, 4, 4, 1, 20, 10)]))
        self.assertEqual(result['interpretedAttempts'], 8)
        self.assertEqual(result['cpuTiming']['estimatedInterpreterNs'], 120)
        top = result['cpuTiming']['opcodesByEstimatedCpu'][0]
        self.assertEqual(top['opcode'], '0f a3')
        self.assertAlmostEqual(top['cpuEstimatedPercent'], 100 * 80 / 120)
        primary = next(r for r in result['opcodes'] if r['opcode'] == '0f')
        self.assertEqual(primary['cpuEstimatedNs'], 0)

    def test_cost_can_prioritize_a_less_frequent_instruction(self):
        result = self.read(profile([op(0x90, 90, 90, 9, 180, 90), op(0xa5, 10, 10, 2, 1020, 1000)]))
        self.assertEqual(result['opcodes'][0]['opcode'], '90')
        self.assertEqual(result['cpuTiming']['opcodesByEstimatedCpu'][0]['opcode'], 'a5')
        self.assertEqual(result['cpuTiming']['estimatedInterpreterNs'], 5900)

    def test_legacy_attempts_do_not_extrapolate_new_samples(self):
        old = profile([op(0x90, 990)], timing=False)
        result = self.read(old, profile([op(0x90, 10, 10, 2, 120, 100)]))
        self.assertEqual(result['opcodes'][0]['attempts'], 1000)
        self.assertEqual(result['opcodes'][0]['cpuLeafAttempts'], 10)
        self.assertEqual(result['cpuTiming']['estimatedInterpreterNs'], 500)
        legacy = self.read(old)
        self.assertFalse(legacy['cpuTiming']['available'])
        self.assertEqual(legacy['cpuTiming']['opcodesByEstimatedCpu'], [])

    def test_fault_samples_and_unknown_work_remain_visible(self):
        result = self.read(profile([op(0x8e, 10, 10, 2, 120, 100, 2, 1)],
                                   unknownAttempts=4, unknownSamples=2, unknownRawNs=220, unknownAdjustedNs=200))
        self.assertEqual(result['cpuTiming']['estimatedInterpreterNs'], 900)
        self.assertEqual(result['cpuTiming']['unknownEstimatedNs'], 400)
        self.assertEqual(result['opcodes'][0]['cpuFaultSamples'], 2)
        self.assertEqual(result['opcodes'][0]['cpuExceptionSamples'], 1)
        self.assertTrue(result['opcodes'][0]['cpuLowSampleCount'])

    def test_invalid_sample_accounting_is_rejected(self):
        for row in [op(0x90, 1, 1, 2), op(0x90, 1, 1, 1, 10, 11),
                    op(0x90, 1, 1, 1, 10, 10, 2), op(0x90, 1, 2, 1)]:
            with self.subTest(row=row), self.assertRaisesRegex(ValueError, 'sample accounting'):
                self.read(profile([row]))
        with self.assertRaisesRegex(ValueError, 'unknown CPU'):
            self.read(profile([op(0x90, 1)], unknownAttempts=0, unknownSamples=1))

    def test_wrong_schema_and_inconsistent_primary_counts_are_rejected(self):
        value = profile([op(0x90, 1)])
        value['cpuTiming']['version'] = 99
        with self.assertRaisesRegex(ValueError, 'schema'):
            self.read(value)
        value = profile([op(0x90, 1)])
        value['interpretedAttempts'] = 2
        with self.assertRaisesRegex(ValueError, 'Primary opcode'):
            self.read(value)

    def test_lock_prefix_accounting(self):
        row=op(0x39,3);row['lockPrefixAttempts']=2
        self.assertEqual(self.read(profile([row]))['opcodes'][0]['lockPrefixAttempts'],2)
        for invalid in [-1,4]:
            row['lockPrefixAttempts']=invalid
            with self.assertRaisesRegex(ValueError,'opcode CPU'):
                self.read(profile([row]))

    def test_interpretation_reasons(self):
        row=op(0x8b,3); row['interpretationReasons']={'warming':2,'replay':1}
        result=self.read(profile([row]),profile([row]))
        self.assertEqual(result['opcodes'][0]['interpretationReasons'],{'warming':4,'replay':2})
        row['interpretationReasons']['replay']=2
        with self.assertRaisesRegex(ValueError,'interpretation reason'):
            self.read(profile([row]))
        row['interpretationReasons']={'warming':4,'replay':-1}
        with self.assertRaisesRegex(ValueError,'interpretation reason'):
            self.read(profile([row]))

if __name__ == '__main__':
    unittest.main()
