"""The usage audit must reject synthetic probes and incomplete Windows traces."""
from copy import deepcopy
from pathlib import Path
import sys
import unittest
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
from verify_windows_hyperv import verify


def fixture():
    registers = []
    for name, reads, writes in ((0x200, 2, 0), (0x90002, 0, 3), (0x90003, 1, 0),
                               (0x90006, 1, 0), (0x90015, 1, 1),
                               (0x90016, 0, 1), (0x40400, 0, 1)):
        registers.append(dict(name=name, reads=reads, writes=writes, failures=0))
    report = dict(schema=1, mode='basic', guestOsId=0x0001040a000065f4,
                  partitionInfoPage=0x4000d001, omittedCounters=0, failures=4,
                  smcccCalls=0, legacyCalls=18,
                  registers=registers,
                  vps=[dict(index=0, tlbiControl=1, syntheticVector=0xfffff80012345000)],
                  calls=[dict(code=0x50, count=8), dict(code=0x51, count=6), dict(code=0x19, count=4)])
    baseline = dict(schema=1, mode='off', guestOsId=0, smcccCalls=0, legacyCalls=0, registers=[])
    return report, baseline


class WindowsHyperV(unittest.TestCase):
    def test_core_use_does_not_hide_unsupported_calls_or_claim_boot(self):
        report, baseline = fixture()
        result = verify(report, baseline)
        self.assertTrue(result['verifiedCoreUsage'])
        self.assertEqual(result['unsupportedCalls'], [dict(code=0x19, count=4)])
        self.assertEqual(result['totalFailures'], 4)
        self.assertFalse(result['guiBootVerified'])
        self.assertFalse(result['performanceBenefitVerified'])

    def test_reference_counter_requires_successful_windows_reads(self):
        report, baseline = fixture()
        self.assertFalse(verify(report, baseline)['referenceCounterObserved'])
        report['referenceTime100ns'] = 12345
        self.assertFalse(verify(report, baseline)['referenceCounterObserved'])
        report['registers'].append(dict(name=0x90004, reads=2, writes=0, failures=0))
        self.assertTrue(verify(report, baseline)['referenceCounterObserved'])
        report['registers'][-1]['failures'] = 1
        self.assertFalse(verify(report, baseline)['referenceCounterObserved'])
        report['registers'][-1]['failures'] = 0
        report['referenceTime100ns'] = 0
        self.assertFalse(verify(report, baseline)['referenceCounterObserved'])
        report['referenceTime100ns'] = 12345
        report['guestOsId'] = 0x5043454d55
        self.assertFalse(verify(report, baseline)['referenceCounterObserved'])

    def test_required_evidence(self):
        report, baseline = fixture()
        changes = [dict(mode='off'), dict(guestOsId=0x5043454d55),
                   dict(guestOsId=0), dict(registers=[]), dict(omittedCounters=1),
                   dict(calls=[]), dict(legacyCalls=0),
                   dict(partitionInfoPage=0), dict(partitionInfoPage=0x4000d003),
                   dict(vps=[]), dict(vps=[dict(index=0, tlbiControl=0, syntheticVector=0)])]
        for change in changes:
            with self.subTest(change=change):
                self.assertFalse(verify(dict(report, **change), baseline)['verifiedCoreUsage'])
        for i in range(len(report['registers'])):
            for key, value in (('failures', 1), ('reads', -1), ('writes', -1)):
                changed = deepcopy(report)
                changed['registers'][i][key] = value
                self.assertFalse(verify(changed, baseline)['verifiedCoreUsage'])
        duplicate = deepcopy(report)
        duplicate['registers'].append(duplicate['registers'][0].copy())
        self.assertFalse(verify(duplicate, baseline)['verifiedCoreUsage'])

    def test_baseline_is_required_to_be_inactive(self):
        report, baseline = fixture()
        for change in (dict(mode='basic'), dict(guestOsId=1), dict(legacyCalls=1),
                       dict(smcccCalls=1), dict(registers=[dict(name=0x200)])):
            self.assertFalse(verify(report, dict(baseline, **change))['verifiedCoreUsage'])


if __name__ == '__main__':
    unittest.main()
