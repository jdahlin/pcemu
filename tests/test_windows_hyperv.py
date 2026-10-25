"""The usage audit must reject synthetic probes and incomplete Windows traces."""
from copy import deepcopy
import json
import subprocess
import tempfile
from pathlib import Path
import sys
import unittest
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
from verify_windows_hyperv import verify, verify_poweroff, verify_reboot


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


def offered_fixture():
    report, baseline = fixture()
    for row in report['calls']:
        row['failures'] = 4 if row['code'] == 0x19 else 0
    report['calls'].append(dict(code=0x5c, count=2, failures=0))
    report.update(mode='vmbus', legacyCalls=20, postedMessages=2, privateInterruptRequests=[2])
    for name in (0xa0010, 0xa0013, 0xa0002):
        report['registers'].append(dict(name=name, reads=0, writes=1, failures=0))
    report['vmbus'] = dict(connected=True, version=0x60000, contacts=1, offerRequests=1,
                          unloads=0, deliveredReplies=4, pendingReplies=0, channelOffers=2,
                          rejectedMessages=0, targetVp=0, targetSint=2, offeredChannels=2,
                          channelOpens=0, channelCloses=0, gpadls=0,
                          packetsReceived=0, packetsSent=0, channelFaults=0)
    report['synic'] = dict(vps=[dict(index=0, control=1, messagePage=0x40001001,
        sints=[dict(index=2, config=8, slotType=0, queued=0)])])
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

    def test_extended_query_needs_success_counters(self):
        report, baseline = fixture()
        report['calls'].append(dict(code=0x8001, count=1))
        report['legacyCalls'] += 1
        self.assertFalse(verify(report, baseline)['extendedCapabilitiesObserved'])
        for row in report['calls']:
            row['failures'] = 4 if row['code'] == 0x19 else 0
        self.assertTrue(verify(report, baseline)['extendedCapabilitiesObserved'])
        report['calls'][-1]['failures'] = 1
        report['failures'] += 1
        self.assertFalse(verify(report, baseline)['extendedCapabilitiesObserved'])
        report['calls'][-1]['failures'] = 2
        self.assertFalse(verify(report, baseline)['verifiedCoreUsage'])

    def test_context_sync_requires_completed_native_barriers(self):
        report, baseline = fixture()
        self.assertFalse(verify(report, baseline)['contextSyncObserved'])
        for row in report['calls']:
            row['failures'] = 0
        report.update(failures=0, contextSyncRequests=4, contextSyncTargetMask=1,
                      instructionContextBarriers=[4])
        self.assertTrue(verify(report, baseline)['contextSyncObserved'])
        self.assertEqual(verify(report, baseline)['unsupportedCalls'], [])
        for change in (dict(contextSyncRequests=0), dict(contextSyncRequests=5),
                       dict(instructionContextBarriers=[0]), dict(instructionContextBarriers=[3]),
                       dict(instructionContextBarriers=[False]), dict(instructionContextBarriers=[]),
                       dict(contextSyncTargetMask=0), dict(contextSyncTargetMask=2)):
            self.assertFalse(verify(dict(report, **change), baseline)['contextSyncObserved'])
        report['calls'][-1]['failures'] = 1
        report['failures'] = 1
        self.assertFalse(verify(report, baseline)['contextSyncObserved'])

    def test_extended_context_sync_counts_are_included(self):
        report, baseline = fixture()
        for row in report['calls']:
            row['failures'] = 0
        report['calls'].append(dict(code=0x1a, count=3, failures=0))
        report.update(failures=0, legacyCalls=21, contextSyncRequests=7,
                      contextSyncTargetMask=1, instructionContextBarriers=[7])
        self.assertTrue(verify(report, baseline)['contextSyncObserved'])
        self.assertTrue(verify(report, baseline)['contextSyncExObserved'])
        report['instructionContextBarriers'] = [4]
        self.assertFalse(verify(report, baseline)['contextSyncObserved'])

    def test_tlb_flush_needs_successful_calls_and_native_completions(self):
        report, baseline = fixture()
        self.assertFalse(verify(report, baseline)['tlbFlushObserved'])
        for row in report['calls']:
            row['failures'] = 4 if row['code'] == 0x19 else 0
        report['calls'] += [dict(code=0x2, count=2, failures=0), dict(code=0x13, count=1, failures=0)]
        report.update(legacyCalls=21, tlbFlushRequests=3, tlbFlushTargetMask=1,
                      tlbFlushFlags=7, translationContextFlushes=[3])
        self.assertTrue(verify(report, baseline)['tlbFlushObserved'])
        for change in (dict(tlbFlushRequests=4), dict(tlbFlushTargetMask=0),
                       dict(tlbFlushTargetMask=2), dict(tlbFlushFlags=8),
                       dict(translationContextFlushes=[2]), dict(translationContextFlushes=[]),
                       dict(translationContextFlushes=[False]), dict(guestOsId=0x5043454d55)):
            self.assertFalse(verify(dict(report, **change), baseline)['tlbFlushObserved'])
        report['calls'][-1]['failures'] = 1
        report['failures'] += 1
        self.assertFalse(verify(report, baseline)['tlbFlushObserved'])

    def test_arm_tlb_drain_is_a_counted_flush(self):
        report, baseline = fixture()
        for row in report['calls']:
            row['failures'] = 4 if row['code'] == 0x19 else 0
        report['calls'].append(dict(code=0xd6, count=2, failures=0))
        report.update(legacyCalls=20, tlbFlushRequests=2, tlbFlushTargetMask=1,
                      tlbFlushFlags=0, translationContextFlushes=[2])
        self.assertTrue(verify(report, baseline)['tlbFlushObserved'])
        report['translationContextFlushes'] = [1]
        self.assertFalse(verify(report, baseline)['tlbFlushObserved'])

    def test_ipi_requires_successful_calls_and_matching_native_requests(self):
        report, baseline = fixture()
        self.assertFalse(verify(report, baseline)['ipiObserved'])
        for row in report['calls']:
            row['failures'] = 4 if row['code'] == 0x19 else 0
        report['calls'] += [dict(code=0xb, count=2, failures=0), dict(code=0x15, count=1, failures=0)]
        report.update(legacyCalls=21, ipiRequests=3, ipiTargetMask=1, ipiVector=0,
                      ipiTargetRequests=[3], softwareInterruptRequests=[3])
        self.assertTrue(verify(report, baseline)['ipiObserved'])
        for change in (dict(ipiRequests=2), dict(ipiTargetMask=2), dict(ipiVector=16),
                       dict(ipiTargetRequests=[0]), dict(ipiTargetRequests=[4]),
                       dict(softwareInterruptRequests=[2]), dict(softwareInterruptRequests=[True]),
                       dict(ipiTargetRequests=[]), dict(guestOsId=0x5043454d55)):
            self.assertFalse(verify(dict(report, **change), baseline)['ipiObserved'])
        report['calls'][-1]['failures'] = 1
        report['failures'] += 1
        self.assertFalse(verify(report, baseline)['ipiObserved'])

    def test_connection_usage_requires_accepted_requests(self):
        report, baseline = fixture()
        for row in report['calls']:
            row['failures'] = 4 if row['code'] == 0x19 else 0
        report['calls'] += [dict(code=0x5c, count=2, failures=0), dict(code=0x5d, count=3, failures=0)]
        report.update(legacyCalls=23, postedMessages=2, signaledEvents=3)
        result = verify(report, baseline)
        self.assertTrue(result['messagePostsObserved'])
        self.assertTrue(result['eventSignalsObserved'])
        self.assertFalse(any(row['code'] in (0x5c, 0x5d) for row in result['unsupportedCalls']))
        for field, counter in (('messagePostsObserved', 'postedMessages'), ('eventSignalsObserved', 'signaledEvents')):
            for value in (None, 0, 1, True):
                self.assertFalse(verify(dict(report, **{counter: value}), baseline)[field])
            self.assertFalse(verify(dict(report, guestOsId=0x5043454d55), baseline)[field])
        report['calls'][-1]['failures'] = 1
        report['failures'] += 1
        self.assertFalse(verify(report, baseline)['eventSignalsObserved'])

    def test_synic_profile_keeps_core_audit_separate_from_delivery_proof(self):
        for mode in ('synic', 'vmbus'):
            report, baseline = fixture()
            report['mode'] = mode
            result = verify(report, baseline)
            self.assertTrue(result['verifiedCoreUsage'])
            self.assertFalse(result['messagePostsObserved'])
            self.assertFalse(result['eventSignalsObserved'])
            self.assertFalse(result['guiBootVerified'])

    def test_vmbus_control_requires_negotiation_offers_and_delivery(self):
        report, baseline = fixture()
        for row in report['calls']:
            row['failures'] = 4 if row['code'] == 0x19 else 0
        report['calls'].append(dict(code=0x5c, count=2, failures=0))
        report.update(mode='vmbus', legacyCalls=20, postedMessages=2, privateInterruptRequests=[2])
        for name in (0xa0010, 0xa0013, 0xa0002):
            report['registers'].append(dict(name=name, reads=0, writes=1, failures=0))
        report['vmbus'] = dict(connected=True, version=0x60000, contacts=1, offerRequests=1,
                              unloads=0, deliveredReplies=2, pendingReplies=0,
                              rejectedMessages=0, targetVp=0, targetSint=2, offeredChannels=0)
        self.assertTrue(verify(report, baseline)['vmbusControlObserved'])
        for key, value in (('connected', False), ('version', 0), ('version', 0x60001),
                           ('contacts', 0), ('offerRequests', 0), ('pendingReplies', 1),
                           ('rejectedMessages', 1), ('deliveredReplies', 1), ('targetVp', 1),
                           ('targetSint', 16), ('contacts', True)):
            changed = deepcopy(report)
            changed['vmbus'][key] = value
            self.assertFalse(verify(changed, baseline)['vmbusControlObserved'])
        for change in (dict(mode='synic'), dict(vmbus=None), dict(postedMessages=0),
                       dict(privateInterruptRequests=[0]), dict(privateInterruptRequests=[True]),
                       dict(guestOsId=0x5043454d55)):
            self.assertFalse(verify(dict(report, **change), baseline)['vmbusControlObserved'])
        for name in (0xa0010, 0xa0013, 0xa0002):
            changed = deepcopy(report)
            changed['registers'] = [row for row in changed['registers'] if row['name'] != name]
            self.assertFalse(verify(changed, baseline)['vmbusControlObserved'])

    def test_offers_are_host_replies_not_extra_guest_posts(self):
        report, baseline = offered_fixture()
        result = verify(report, baseline)
        self.assertTrue(result['vmbusControlObserved'])
        self.assertTrue(result['vmbusOffersConsumedObserved'])
        self.assertEqual(report['postedMessages'], 2)
        self.assertEqual(report['vmbus']['deliveredReplies'], 4)
        for key, value in (('deliveredReplies', 3), ('channelOffers', True), ('channelOffers', 9),
                           ('offeredChannels', 3), ('channelOpens', 1), ('gpadls', 1),
                           ('packetsReceived', 1), ('packetsSent', 1), ('channelFaults', 1)):
            changed = deepcopy(report)
            changed['vmbus'][key] = value
            self.assertFalse(verify(changed, baseline)['vmbusControlObserved'], (key, value))

    def test_accepted_synic_queue_is_not_guest_consumption(self):
        report, baseline = offered_fixture()
        for key, value in (('slotType', 1), ('queued', 1), ('slotType', False),
                           ('config', 0x10008), ('config', -131073), ('config', None)):
            changed = deepcopy(report)
            changed['synic']['vps'][0]['sints'][0][key] = value
            result = verify(changed, baseline)
            self.assertTrue(result['vmbusControlObserved'])
            self.assertFalse(result['vmbusOffersConsumedObserved'])
        for change in ({}, dict(vps=[]), dict(vps=[dict(index=True)]), dict(vps=None)):
            self.assertFalse(verify(dict(report, synic=change), baseline)['vmbusOffersConsumedObserved'])
        for key, value in (('control', True), ('control', 0), ('messagePage', 0), ('messagePage', 0x40001003)):
            changed = deepcopy(report)
            changed['synic']['vps'][0][key] = value
            self.assertFalse(verify(changed, baseline)['vmbusOffersConsumedObserved'])
        duplicate = deepcopy(report)
        duplicate['synic']['vps'] *= 2
        self.assertFalse(verify(duplicate, baseline)['vmbusOffersConsumedObserved'])

    def test_timer_delivery_needs_programming_and_successful_delivery(self):
        report, baseline = fixture()
        report['syntheticTimerDeliveries'] = 10
        self.assertFalse(verify(report, baseline)['syntheticTimerDeliveryObserved'])
        for name in (0xb0000, 0xb0001):
            report['registers'].append(dict(name=name, reads=0, writes=2, failures=0))
        self.assertTrue(verify(report, baseline)['syntheticTimerDeliveryObserved'])
        for value in (0, True, -1, '10'):
            self.assertFalse(verify(dict(report, syntheticTimerDeliveries=value), baseline)['syntheticTimerDeliveryObserved'])
        report['registers'][-1]['failures'] = 1
        self.assertFalse(verify(report, baseline)['syntheticTimerDeliveryObserved'])

    def test_heartbeat_needs_real_responses_not_offers_or_host_probes(self):
        report, baseline = offered_fixture()
        self.assertFalse(verify(report, baseline)['heartbeatResponseObserved'])
        report['calls'].append(dict(code=0x5d, count=1, failures=0))
        report.update(legacyCalls=21, signaledEvents=1)
        report['vmbus'].update(channelOpens=1, packetsReceived=2, packetsSent=2)
        report['heartbeat'] = dict(state='ready', frameworkMajor=3, heartbeatMajor=3,
                                   negotiations=1, probes=1, responses=1, timeouts=0,
                                   rejectedResponses=0)
        result = verify(report, baseline)
        self.assertTrue(result['heartbeatResponseObserved'])
        self.assertFalse(result['guiBootVerified'])
        for malformed in (None, [], True):
            self.assertFalse(verify(dict(report, heartbeat=malformed), baseline)['heartbeatResponseObserved'])
        for key, value in (('negotiations', 0), ('responses', 0), ('responses', True),
                           ('probes', 0), ('frameworkMajor', True), ('heartbeatMajor', 2),
                           ('state', 'closed'), ('timeouts', -1)):
            changed = deepcopy(report)
            changed['heartbeat'][key] = value
            self.assertFalse(verify(changed, baseline)['heartbeatResponseObserved'], (key, value))
        for key, value in (('packetsReceived', 1), ('packetsSent', 1), ('channelOpens', 0), ('connected', False)):
            changed = deepcopy(report)
            changed['vmbus'][key] = value
            self.assertFalse(verify(changed, baseline)['heartbeatResponseObserved'])
        self.assertFalse(verify(dict(report, signaledEvents=0), baseline)['heartbeatResponseObserved'])

    def test_timesync_ack_requires_guest_channel_responses(self):
        report, baseline = offered_fixture()
        self.assertFalse(verify(report, baseline)['timesyncAcknowledgmentObserved'])
        report['calls'].append(dict(code=0x5d, count=1, failures=0))
        report.update(legacyCalls=21, signaledEvents=1)
        report['vmbus'].update(channelOpens=1, gpadls=1, packetsReceived=2, packetsSent=2)
        report['timesync'] = dict(state='ready', frameworkMajor=3, timesyncMajor=4,
            negotiations=1, samples=1, acknowledgments=1, timeouts=0, rejectedResponses=0)
        self.assertTrue(verify(report, baseline)['timesyncAcknowledgmentObserved'])
        self.assertFalse(verify(report, baseline)['guestClockAdjustmentVerified'])
        for malformed in (None, [], True):
            self.assertFalse(verify(dict(report, timesync=malformed), baseline)['timesyncAcknowledgmentObserved'])
        for key, value in (('acknowledgments', 0), ('acknowledgments', True), ('acknowledgments', 2),
                           ('samples', 0), ('negotiations', 0), ('frameworkMajor', True),
                           ('timesyncMajor', 3), ('state', 'closed'), ('timeouts', -1)):
            changed = deepcopy(report)
            changed['timesync'][key] = value
            self.assertFalse(verify(changed, baseline)['timesyncAcknowledgmentObserved'], (key, value))
        for key, value in (('channelOpens', 0), ('gpadls', 0), ('packetsReceived', 1),
                           ('packetsSent', 1), ('connected', False)):
            changed = deepcopy(report)
            changed['vmbus'][key] = value
            self.assertFalse(verify(changed, baseline)['timesyncAcknowledgmentObserved'])
        self.assertFalse(verify(dict(report, signaledEvents=0), baseline)['timesyncAcknowledgmentObserved'])
        self.assertFalse(verify(dict(report, guestOsId=0), baseline)['timesyncAcknowledgmentObserved'])

    def test_shutdown_acceptance_is_not_terminal_poweroff(self):
        report, baseline = offered_fixture()
        self.assertFalse(verify(report, baseline)['shutdownAcceptedObserved'])
        report['calls'].append(dict(code=0x5d, count=1, failures=0))
        report.update(legacyCalls=21, signaledEvents=1)
        report['vmbus'].update(channelOpens=1, gpadls=1, packetsReceived=2, packetsSent=2)
        report['shutdown'] = dict(state='accepted', frameworkMajor=3, shutdownMajor=3, shutdownMinor=2,
            negotiations=1, requests=1, accepted=1, denied=0, timeouts=0, rejectedResponses=0, lastStatus=0)
        result = verify(report, baseline)
        self.assertTrue(result['shutdownAcceptedObserved'])
        self.assertFalse(result['guestPowerOffVerified'])
        for malformed in (None, [], True):
            self.assertFalse(verify(dict(report, shutdown=malformed), baseline)['shutdownAcceptedObserved'])
        for key, value in (('requests', 0), ('accepted', 0), ('accepted', True), ('accepted', 2),
                           ('denied', 1), ('lastStatus', 1), ('negotiations', 0), ('frameworkMajor', True),
                           ('shutdownMinor', 1), ('state', 'ready'), ('timeouts', -1)):
            changed = deepcopy(report)
            changed['shutdown'][key] = value
            self.assertFalse(verify(changed, baseline)['shutdownAcceptedObserved'], (key, value))
        log = 'Hyper-V guest shutdown requested\nUEFI run complete: exits=123 state=power-off\n'
        self.assertTrue(verify_poweroff(result, log, 0))
        for bad in ('', log.replace('power-off', 'reset-requested'),
                    log.replace('requested', 'unavailable'), log + log,
                    'UEFI run complete: state=power-off\nHyper-V guest shutdown requested\n'):
            self.assertFalse(verify_poweroff(result, bad, 0))
        for code in (1, -15, False, None):
            self.assertFalse(verify_poweroff(result, log, code))
        self.assertFalse(verify_poweroff(dict(result, shutdownAcceptedObserved=False), log, 0))
        with tempfile.TemporaryDirectory() as directory:
            directory = Path(directory)
            for name, data in (('report.json', report), ('baseline.json', baseline)):
                (directory / name).write_text(json.dumps(data))
            command = [sys.executable, str(Path(__file__).resolve().parents[1] / 'tools/verify_windows_hyperv.py'),
                       str(directory / 'report.json'), '--baseline', str(directory / 'baseline.json'),
                       '--shutdown-host-log', str(directory / 'host.log'), '--runner-exit-code', '0']
            for host_log, expected in ((log, 0), (log.replace('power-off', 'reset-requested'), 1)):
                (directory / 'host.log').write_text(host_log)
                completed = subprocess.run(command, capture_output=True, text=True, timeout=5)
                self.assertEqual(completed.returncode, expected, completed.stderr)
                self.assertEqual(json.loads(completed.stdout)['guestPowerOffVerified'], expected == 0)

        for key, value in (('gpadls', 0), ('packetsReceived', 1), ('packetsSent', 1), ('connected', False)):
            changed = deepcopy(report)
            changed['vmbus'][key] = value
            self.assertFalse(verify(changed, baseline)['shutdownAcceptedObserved'])

    def test_reboot_requires_reset_and_fresh_windows_service_use(self):
        before_result = dict(shutdownAcceptedObserved=True)
        after_result = dict(verifiedCoreUsage=True, heartbeatResponseObserved=True, timesyncAcknowledgmentObserved=True)
        before = dict(guestOsId=0x1040a000065f4, shutdown=dict(requestFlags=2))
        after = dict(guestOsId=before['guestOsId'], shutdown=dict(state='ready', requests=0, accepted=0, negotiations=1, frameworkMajor=3, shutdownMajor=3, shutdownMinor=2, requestFlags=0))
        log = ('Hyper-V guest reboot requested\nUEFI run complete: exits=123 state=reset-requested\n'
               'UEFI restarting after guest reset: reboot=1\n')
        self.assertTrue(verify_reboot(before_result, after_result, before, after, log))
        for key in after_result:
            self.assertFalse(verify_reboot(before_result, dict(after_result, **{key: False}), before, after, log))
        for flags in (0, 1, True, None):
            self.assertFalse(verify_reboot(before_result, after_result, dict(before, shutdown=dict(requestFlags=flags)), after, log))
        for key, value in (('state', 'accepted'), ('requests', 1), ('requests', False), ('accepted', 1), ('negotiations', 0), ('shutdownMinor', 1), ('requestFlags', 2), ('frameworkMajor', True)):
            changed = deepcopy(after)
            changed['shutdown'][key] = value
            self.assertFalse(verify_reboot(before_result, after_result, before, changed, log))
        self.assertFalse(verify_reboot(before_result, after_result, before, dict(after, guestOsId=123), log))
        self.assertFalse(verify_reboot(dict(shutdownAcceptedObserved=False), after_result, before, after, log))
        for bad in ('', log + log, log.replace('reset-requested', 'power-off'), log.replace('reboot=1', 'reboot=2'),
                    log.replace('Hyper-V guest reboot requested\n', '') + 'Hyper-V guest reboot requested\n'):
            self.assertFalse(verify_reboot(before_result, after_result, before, after, bad))

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
