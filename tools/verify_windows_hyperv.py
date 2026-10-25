#!/usr/bin/env python3
"""Audit actual Windows ARM64 use of implemented Hyper-V interfaces.

Core use, consumed VMBus offers, timer delivery and heartbeat responses have
separate evidence gates. These counters do not prove GUI boot or a performance
benefit. Unsupported call attempts remain visible even when core use passes.
"""
import argparse
import hashlib
import json
import math
import re
from datetime import datetime
from pathlib import Path


def verify(report, baseline, kernel_build=26100):
    errors = []
    def require(value, message):
        if not value:
            errors.append(message)
    require(report.get('schema') == 1 and report.get('mode') in ('basic', 'synic', 'vmbus'), 'Expected enabled Hyper-V counters')
    identity = report.get('guestOsId', 0)
    require(type(identity) is int and identity >= 0, 'Invalid guest identity')
    identity = identity if type(identity) is int and identity >= 0 else 0
    require((identity >> 48) & 0xffff == 1 and (identity >> 40) & 255 == 4
            and (identity >> 32) & 255 == 10 and identity & 65535 == kernel_build,
            'Identity does not match the expected Microsoft Windows NT kernel')
    registers = {}
    for row in report.get('registers', []):
        name = row.get('name')
        require(type(name) is int and name not in registers, 'Invalid or duplicate register counter')
        registers[name] = row
        require(all(type(row.get(k)) is int and row[k] >= 0 for k in ('reads', 'writes', 'failures')),
                'Invalid register operation counts')
    for name, operation in ((0x200, 'reads'), (0x90002, 'writes'), (0x90003, 'reads'),
                            (0x90006, 'reads'), (0x90015, 'reads'), (0x90015, 'writes'),
                            (0x90016, 'writes'), (0x40400, 'writes')):
        row = registers.get(name, {})
        require(type(row.get(operation)) is int and row[operation] > 0 and row.get('failures') == 0,
                f'Unverified successful {operation} of register {name:#x}')
    page = report.get('partitionInfoPage', 0)
    require(type(page) is int and 4096 < page < (1 << 64) and page & 4095 == 1, 'Partition info page is not enabled')
    vps = report.get('vps', [])
    require(bool(vps) and vps[0].get('index') == 0 and vps[0].get('tlbiControl') == 1,
            'Boot VP has not opted into TLBI control')
    vector = vps[0].get('syntheticVector', 0) if vps else 0
    require(type(vector) is int and vector > 0 and vector & 2047 == 0,
            'Boot VP has not registered an aligned synthetic vector')
    require(report.get('omittedCounters') == 0, 'Counters were omitted')
    calls = report.get('calls', [])
    counts = {}
    for row in calls:
        code, count = row.get('code'), row.get('count')
        require(type(code) is int and 0 <= code <= 65535 and code not in counts,
                'Invalid or duplicate hypercall code')
        require(type(count) is int and count >= 0, 'Invalid hypercall count')
        counts[code] = count if type(count) is int and count >= 0 else 0
    if any('failures' in row for row in calls):
        require(all(type(row.get('failures')) is int and 0 <= row['failures'] <= counts.get(row.get('code'), 0)
                    for row in calls), 'Invalid per-call failure counts')
        require(sum(row.get('failures', 0) for row in calls
                    if type(row.get('failures', 0)) is int) == report.get('failures'),
                'Per-call and total failures disagree')
    require(counts.get(0x50, 0) > 0 and counts.get(0x51, 0) > 0, 'Missing register hypercall invocations')
    abis = [report.get('smcccCalls'), report.get('legacyCalls')]
    require(all(type(n) is int and n >= 0 for n in abis)
            and sum(abis) == sum(counts.values()), 'Hypercall totals disagree')
    require(baseline.get('schema') == 1 and baseline.get('mode') == 'off', 'Expected off-mode baseline')
    require(baseline.get('guestOsId') == 0 and baseline.get('smcccCalls') == 0
            and baseline.get('legacyCalls') == 0 and not baseline.get('registers'),
            'Off-mode Windows baseline unexpectedly used Hyper-V registers')
    unsupported = [row for row in calls if row.get('code') not in (0x50, 0x51, 0x8001)
                   and not (row.get('code') in (0x2, 0x13, 0xd6, 0x19, 0x1a, 0xb, 0x15, 0x5c, 0x5d) and row.get('failures') == 0)]
    clock = registers.get(0x90004, {})
    last_time = report.get('referenceTime100ns', 0)
    clock_observed = (not errors and type(clock.get('reads')) is int
                      and clock['reads'] >= 2 and clock.get('failures') == 0
                      and type(last_time) is int and last_time > 0)
    extended = next((row for row in calls if row.get('code') == 0x8001), {})
    query_observed = (not errors and counts.get(0x8001, 0) > 0 and extended.get('failures') == 0)
    sync_calls = [row for row in calls if row.get('code') in (0x19, 0x1a)]
    barriers = report.get('instructionContextBarriers', [])
    sync_requests = report.get('contextSyncRequests', 0)
    sync_mask = report.get('contextSyncTargetMask', 0)
    sync_observed = (not errors and type(sync_requests) is int and sync_requests > 0
                     and counts.get(0x19, 0) + counts.get(0x1a, 0) == sync_requests
                     and all(row.get('failures') == 0 for row in sync_calls)
                     and type(sync_mask) is int and 0 < sync_mask < (1 << len(vps))
                     and type(barriers) is list and len(barriers) == len(vps)
                     and all(type(n) is int and n >= 0 for n in barriers)
                     and sum(barriers) >= sync_requests)
    flush_calls = [row for row in calls if row.get('code') in (0x2, 0x13, 0xd6)]
    flush_requests = report.get('tlbFlushRequests', 0)
    flush_mask = report.get('tlbFlushTargetMask', 0)
    flush_flags = report.get('tlbFlushFlags', 0)
    flushes = report.get('translationContextFlushes', [])
    flush_observed = (not errors and type(flush_requests) is int and flush_requests > 0
                      and counts.get(0x2, 0) + counts.get(0x13, 0) + counts.get(0xd6, 0) == flush_requests
                      and all(row.get('failures') == 0 for row in flush_calls)
                      and type(flush_mask) is int and 0 < flush_mask < (1 << len(vps))
                      and type(flush_flags) is int and 0 <= flush_flags <= 7
                      and type(flushes) is list and len(flushes) == len(vps)
                      and all(type(n) is int and n >= 0 for n in flushes)
                      and sum(flushes) >= flush_requests)
    ipi_calls = [row for row in calls if row.get('code') in (0xb, 0x15)]
    ipi_requests = report.get('ipiRequests', 0)
    ipi_targets = report.get('ipiTargetRequests', [])
    ipi_native = report.get('softwareInterruptRequests', [])
    ipi_mask, ipi_vector = report.get('ipiTargetMask', 0), report.get('ipiVector', -1)
    ipi_observed = (not errors and type(ipi_requests) is int and ipi_requests > 0
                    and counts.get(0xb, 0) + counts.get(0x15, 0) == ipi_requests
                    and all(row.get('failures') == 0 for row in ipi_calls)
                    and type(ipi_mask) is int and 0 <= ipi_mask < (1 << len(vps))
                    and type(ipi_vector) is int and 0 <= ipi_vector < 16
                    and type(ipi_targets) is list and len(ipi_targets) == len(vps)
                    and all(type(n) is int and 0 <= n <= ipi_requests for n in ipi_targets)
                    and type(ipi_native) is list and len(ipi_native) == len(vps)
                    and all(type(n) is int and n >= 0 for n in ipi_native)
                    and any(ipi_targets) and ipi_native == ipi_targets)
    def connection_observed(code, counter):
        row = next((row for row in calls if row.get('code') == code), {})
        accepted = report.get(counter)
        return (not errors and type(accepted) is int and accepted > 0
                and accepted == counts.get(code) and row.get('failures') == 0)
    message_posts_observed = connection_observed(0x5c, 'postedMessages')
    event_signals_observed = connection_observed(0x5d, 'signaledEvents')
    bus = report.get('vmbus', {})
    if type(bus) is not dict:
        bus = {}
    fields = ('contacts', 'offerRequests', 'unloads', 'deliveredReplies', 'pendingReplies',
              'rejectedMessages', 'targetVp', 'targetSint', 'version')
    bus_counts_valid = all(type(bus.get(field)) is int and bus[field] >= 0 for field in fields)
    synic_configured = all(registers.get(name, {}).get('writes', 0) > 0
                           and registers.get(name, {}).get('failures') == 0
                           for name in (0xa0010, 0xa0013))
    # One incoming RequestOffers produces N offers plus AllOffersDelivered.
    # Older empty-inventory snapshots have no channel counters. Without typed
    # reply counters, conservatively audit only snapshots before channel I/O.
    bootstrap_replies = None
    if bus_counts_valid:
        offers = bus.get('channelOffers', 0)
        inventory = bus.get('offeredChannels', 0)
        channels_valid = (type(offers) is int and 0 <= offers <= 8 * bus['offerRequests']
                          and type(inventory) is int and 0 <= inventory <= min(offers, 8))
        if 'channelOffers' not in bus:
            channels_valid = channels_valid and inventory == 0 and all(
                key not in bus or (type(bus[key]) is int and bus[key] == 0)
                for key in ('channelOpens', 'channelCloses', 'gpadls', 'packetsReceived',
                            'packetsSent', 'channelFaults'))
        else:
            channels_valid = channels_valid and all(type(bus.get(key)) is int and bus[key] == 0
                for key in ('channelOpens', 'channelCloses', 'gpadls', 'packetsReceived',
                            'packetsSent', 'channelFaults'))
        if channels_valid:
            bootstrap_replies = bus['contacts'] + bus['offerRequests'] + bus['unloads'] + offers
    typed_names = ('deliveredContactReplies', 'deliveredOfferReplies',
                   'deliveredOffersDoneReplies', 'deliveredUnloadReplies', 'deliveredChannelReplies')
    if bus_counts_valid and any(name in bus for name in typed_names):
        # New snapshots count only successful SynIC publication, by reply type.
        # Channel traffic must not masquerade as contact/offer completion.
        valid = (all(type(bus.get(name)) is int and bus[name] >= 0 for name in typed_names)
                 and type(bus.get('channelOffers')) is int
                 and 0 <= bus['channelOffers'] <= 8 * bus['offerRequests']
                 and type(bus.get('offeredChannels')) is int
                 and 0 <= bus['offeredChannels'] <= min(bus['channelOffers'], 8)
                 and all(type(bus.get(k)) is int and bus[k] >= 0 for k in
                         ('channelOpens', 'channelCloses', 'gpadls', 'packetsReceived', 'packetsSent'))
                 and type(bus.get('channelFaults')) is int and bus['channelFaults'] == 0)
        if valid:
            valid = (sum(bus[name] for name in typed_names) == bus['deliveredReplies']
                     and bus['deliveredContactReplies'] == bus['contacts']
                     and bus['deliveredOfferReplies'] == bus['channelOffers']
                     and bus['deliveredOffersDoneReplies'] == bus['offerRequests']
                     and bus['deliveredUnloadReplies'] == bus['unloads'])
        bootstrap_replies = bus['deliveredReplies'] if valid else None
    vmbus_observed = False
    if bus_counts_valid and message_posts_observed and report.get('mode') == 'vmbus':
        vp, sint = bus['targetVp'], bus['targetSint']
        private_irqs = report.get('privateInterruptRequests', [])
        sint_row = registers.get(0xa0000 + sint, {})
        vmbus_observed = (bus.get('connected') is True and bus['version'] in
                          (0x50000, 0x50001, 0x50002, 0x50003, 0x60000)
                          and bus['contacts'] > 0 and bus['offerRequests'] > 0
                          and bus['rejectedMessages'] == 0 and bus['pendingReplies'] == 0
                          and bootstrap_replies is not None and bus['deliveredReplies'] == bootstrap_replies
                          and report['postedMessages'] >= bus['contacts'] + bus['offerRequests'] + bus['unloads']
                          and 0 <= vp < len(vps) and 0 <= sint < 16 and synic_configured
                          and sint_row.get('writes', 0) > 0 and sint_row.get('failures') == 0
                          and type(private_irqs) is list and len(private_irqs) == len(vps)
                          and type(private_irqs[vp]) is int and private_irqs[vp] > 0)
    offers_consumed = False
    synic = report.get('synic', {})
    if vmbus_observed and type(synic) is dict and type(synic.get('vps')) is list:
        targets = [row for row in synic['vps'] if type(row) is dict and type(row.get('index')) is int and row['index'] == bus['targetVp']]
        if len(targets) == 1 and type(targets[0].get('sints')) is list:
            slots = [row for row in targets[0]['sints'] if type(row) is dict and type(row.get('index')) is int and row['index'] == bus['targetSint']]
            if len(slots) == 1:
                slot = slots[0]
                page = targets[0].get('messagePage')
                config = slot.get('config')
                offers_consumed = (type(targets[0].get('control')) is int and targets[0]['control'] == 1 and
                    type(page) is int and 4096 < page < (1 << 64) and page & 4095 == 1 and
                    type(config) is int and 0 <= config < (1 << 64) and not config & 0x10000 and
                    type(slot.get('slotType')) is int and slot['slotType'] == 0 and
                    type(slot.get('queued')) is int and slot['queued'] == 0)
    timer_delivery = (not errors and type(report.get('syntheticTimerDeliveries')) is int
                      and report['syntheticTimerDeliveries'] > 0
                      and any(registers.get(0xb0000 + 2 * i, {}).get('writes', 0) > 0
                              and registers.get(0xb0000 + 2 * i, {}).get('failures') == 0
                              and registers.get(0xb0001 + 2 * i, {}).get('writes', 0) > 0
                              and registers.get(0xb0001 + 2 * i, {}).get('failures') == 0
                              for i in range(4)))
    heartbeat = report.get('heartbeat', {})
    heartbeat_response = (not errors and type(heartbeat) is dict and type(bus) is dict
        and report.get('mode') == 'vmbus' and bus.get('connected') is True
        and heartbeat.get('state') in ('ready', 'wait-heartbeat', 'unresponsive')
        and all(type(heartbeat.get(k)) is int and heartbeat[k] >= 0
                for k in ('negotiations', 'probes', 'responses', 'timeouts', 'rejectedResponses'))
        and 0 < heartbeat['responses'] <= heartbeat['probes'] and heartbeat['negotiations'] > 0
        and type(heartbeat.get('frameworkMajor')) is int and heartbeat['frameworkMajor'] in (1, 3)
        and type(heartbeat.get('heartbeatMajor')) is int and heartbeat['heartbeatMajor'] in (1, 3)
        and all(type(bus.get(k)) is int and bus[k] > 0 for k in
                ('channelOpens', 'packetsReceived', 'packetsSent'))
        and bus['packetsReceived'] >= heartbeat['responses'] + 1
        and bus['packetsSent'] >= heartbeat['probes'] + heartbeat['negotiations']
        and event_signals_observed)
    timesync = report.get('timesync', {})
    timesync_ack = (not errors and type(timesync) is dict
        and report.get('mode') == 'vmbus' and bus.get('connected') is True
        and timesync.get('state') in ('ready', 'wait-sample')
        and all(type(timesync.get(k)) is int and timesync[k] >= 0
                for k in ('negotiations', 'samples', 'acknowledgments', 'timeouts', 'rejectedResponses'))
        and timesync['negotiations'] > 0 and 0 < timesync['acknowledgments'] <= timesync['samples']
        and type(timesync.get('frameworkMajor')) is int and timesync['frameworkMajor'] in (1, 3)
        and type(timesync.get('timesyncMajor')) is int and timesync['timesyncMajor'] == 4
        and all(type(bus.get(k)) is int and bus[k] > 0 for k in
                ('channelOpens', 'gpadls', 'packetsReceived', 'packetsSent'))
        and bus['packetsReceived'] >= timesync['acknowledgments'] + timesync['negotiations']
        and bus['packetsSent'] >= timesync['samples'] + timesync['negotiations']
        and event_signals_observed)
    shutdown = report.get('shutdown', {})
    shutdown_accepted = (not errors and type(shutdown) is dict
        and report.get('mode') == 'vmbus' and bus.get('connected') is True
        and shutdown.get('state') == 'accepted'
        and all(type(shutdown.get(k)) is int and shutdown[k] >= 0
                for k in ('negotiations', 'requests', 'accepted', 'denied', 'timeouts', 'rejectedResponses', 'lastStatus'))
        and shutdown['negotiations'] > 0 and shutdown['accepted'] > 0
        and shutdown['requests'] >= shutdown['accepted'] + shutdown['denied']
        and shutdown['lastStatus'] == 0
        and type(shutdown.get('frameworkMajor')) is int and shutdown['frameworkMajor'] in (1, 3)
        and type(shutdown.get('shutdownMajor')) is int and shutdown['shutdownMajor'] == 3
        and type(shutdown.get('shutdownMinor')) is int and shutdown['shutdownMinor'] == 2
        and all(type(bus.get(k)) is int and bus[k] > 0 for k in
                ('channelOpens', 'gpadls', 'packetsReceived', 'packetsSent'))
        and bus['packetsReceived'] >= shutdown['accepted'] + shutdown['denied'] + shutdown['negotiations']
        and bus['packetsSent'] >= shutdown['requests'] + shutdown['negotiations']
        and event_signals_observed)
    return {'vmbusControlObserved': vmbus_observed,
            'vmbusOffersConsumedObserved': offers_consumed,
            'syntheticTimerDeliveryObserved': timer_delivery,
            'heartbeatResponseObserved': heartbeat_response,
            'timesyncAcknowledgmentObserved': timesync_ack,
            'shutdownAcceptedObserved': shutdown_accepted, 'guestPowerOffVerified': False, 'guestRebootVerified': False, 'guestClockAdjustmentVerified': False,
            'messagePostsObserved': message_posts_observed, 'eventSignalsObserved': event_signals_observed,
            'ipiObserved': ipi_observed, 'tlbFlushObserved': flush_observed, 'contextSyncExObserved': sync_observed and counts.get(0x1a, 0) > 0,
            'contextSyncObserved': sync_observed, 'extendedCapabilitiesObserved': query_observed, 'referenceCounterObserved': clock_observed, 'verifiedCoreUsage': not errors, 'kernelBuild': kernel_build, 'errors': errors,
            'unsupportedCalls': unsupported, 'totalFailures': report.get('failures'),
            'guiBootVerified': False, 'performanceBenefitVerified': False}


def verify_poweroff(result, host_log, runner_exit_code):
    """Require a guest acceptance plus the same runner's terminal power state."""
    if not result.get('shutdownAcceptedObserved') or type(runner_exit_code) is not int or runner_exit_code != 0:
        return False
    lines = host_log.splitlines()
    requests = [i for i, line in enumerate(lines) if line == 'Hyper-V guest shutdown requested']
    terminals = [(i, line) for i, line in enumerate(lines) if line.startswith('UEFI run complete:')]
    return (len(requests) == 1 and len(terminals) == 1 and requests[0] < terminals[0][0]
            and terminals[0][1].endswith(' state=power-off'))


def verify_reboot(before_result, after_result, before, after, host_log):
    """Require accepted restart, platform reset, then fresh Windows service use."""
    shutdown = before.get('shutdown', {})
    fresh = after.get('shutdown', {})
    if (not before_result.get('shutdownAcceptedObserved') or type(shutdown) is not dict
            or type(shutdown.get('requestFlags')) is not int or shutdown['requestFlags'] not in (2, 3)
            or not after_result.get('verifiedCoreUsage')
            or not after_result.get('heartbeatResponseObserved')
            or not after_result.get('timesyncAcknowledgmentObserved')
            or before.get('guestOsId') != after.get('guestOsId')
            or type(fresh) is not dict or fresh.get('state') != 'ready'
            or type(fresh.get('frameworkMajor')) is not int or fresh['frameworkMajor'] not in (1, 3)
            or type(fresh.get('shutdownMajor')) is not int or fresh['shutdownMajor'] != 3
            or type(fresh.get('shutdownMinor')) is not int or fresh['shutdownMinor'] != 2
            or type(fresh.get('requestFlags')) is not int or fresh['requestFlags'] != 0
            or type(fresh.get('requests')) is not int or fresh['requests'] != 0
            or type(fresh.get('accepted')) is not int or fresh['accepted'] != 0
            or type(fresh.get('negotiations')) is not int or fresh['negotiations'] <= 0):
        return False
    lines = host_log.splitlines()
    requests = [i for i, line in enumerate(lines) if line == 'Hyper-V guest reboot requested']
    resets = [i for i, line in enumerate(lines) if line.startswith('UEFI run complete:') and line.endswith(' state=reset-requested')]
    restarts = [i for i, line in enumerate(lines) if line == 'UEFI restarting after guest reset: reboot=1']
    return len(requests) == len(resets) == len(restarts) == 1 and requests[0] < resets[0] < restarts[0]


def verify_clock(result, clock):
    """Require actual isolated correction, never a host-probe restoration."""
    if (not result.get('verifiedCoreUsage') or not result.get('timesyncAcknowledgmentObserved')
            or type(clock) is not dict or type(clock.get('schema')) is not int or clock['schema'] != 1
            or clock.get('kind') != 'pcemu-hyperv-clock'
            or any(clock.get(k) is not True for k in ('stepRequested', 'adjustmentObserved',
                                                       'ntpClientDisabled', 'ntpClientDisabledAfter'))
            or clock.get('restoredByProbe') is not False or clock.get('error') is not None
            or clock.get('cleanupErrors') != []):
        return False
    source = clock.get('sourceAfter')
    if (type(source) is not dict or type(source.get('exitCode')) is not int or source['exitCode'] != 0
            or source.get('lines') != ['VM IC Time Synchronization Provider']):
        return False
    def sample(row):
        if type(row) is not dict:
            raise ValueError('Missing sample')
        for key in ('offsetSeconds', 'roundTripMilliseconds'):
            if type(row.get(key)) not in (int, float) or not math.isfinite(row[key]):
                raise ValueError('Invalid measurement')
        if not 0 <= row['roundTripMilliseconds'] <= 5000:
            raise ValueError('Unbounded request')
        times = []
        for key in ('hostUtc', 'guestUtc'):
            if type(row.get(key)) is not str:
                raise ValueError('Missing timestamp')
            text = row[key]
            # .NET round-trip timestamps carry seven fractional digits.
            if not re.fullmatch(r'\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}\.\d{1,7}Z', text):
                raise ValueError('Expected UTC round-trip timestamp')
            prefix, fraction = text[:-1].split('.')
            value = datetime.fromisoformat(prefix + '.' + fraction[:6].ljust(6, '0') + '+00:00')
            if value.tzinfo is None or value.utcoffset().total_seconds() != 0:
                raise ValueError('Expected UTC')
            times.append(value)
        if abs((times[1] - times[0]).total_seconds() - row['offsetSeconds']) > 0.001:
            raise ValueError('Inconsistent offset')
        return times[0]
    try:
        before, stepped, after = (clock[k] for k in ('before', 'stepped', 'after'))
        previous = sample(before)
        stamp = sample(stepped)
        if stamp < previous or abs(stepped['offsetSeconds']) < 60:
            return False
        if not -125 <= stepped['offsetSeconds'] - before['offsetSeconds'] <= -115:
            return False
        previous = stamp
        rows = clock.get('samples')
        if type(rows) is not list or not 1 <= len(rows) <= 12 or rows[-1] != after:
            return False
        for row in rows:
            stamp = sample(row)
            if stamp < previous:
                return False
            previous = stamp
        return abs(after['offsetSeconds']) <= 3 and after['roundTripMilliseconds'] <= 2000
    except (KeyError, ValueError, TypeError, OverflowError):
        return False


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('report', type=Path)
    parser.add_argument('--baseline', type=Path, required=True)
    parser.add_argument('--clock-report', type=Path, help='Isolated installed-Windows clock-step report')
    parser.add_argument('--kernel-build', type=int, default=26100)
    parser.add_argument('--shutdown-host-log', type=Path)
    parser.add_argument('--runner-exit-code', type=int)
    parser.add_argument('--reboot-report', type=Path, help='Fresh Windows counter snapshot after guest reset')
    parser.add_argument('--reboot-host-log', type=Path)
    args = parser.parse_args()
    if (args.reboot_report is None) != (args.reboot_host_log is None):
        parser.error('Supply both reboot report and reboot host log')
    if (args.shutdown_host_log is None) != (args.runner_exit_code is None):
        parser.error('Supply both shutdown host log and runner exit code')
    raw = {}
    for name, path in (('report', args.report), ('baseline', args.baseline)):
        if path.stat().st_size > 1024 * 1024:
            parser.error('Expected bounded counter snapshots')
        raw[name] = path.read_bytes()
    result = verify(json.loads(raw['report']), json.loads(raw['baseline']), args.kernel_build)
    result['sha256'] = {name: hashlib.sha256(data).hexdigest() for name, data in raw.items()}
    if args.shutdown_host_log is not None:
        if args.shutdown_host_log.stat().st_size > 4 * 1024 * 1024:
            parser.error('Expected bounded shutdown host log')
        host_raw = args.shutdown_host_log.read_bytes()
        result['guestPowerOffVerified'] = verify_poweroff(result, host_raw.decode('utf-8'), args.runner_exit_code)
        result['sha256']['shutdownHostLog'] = hashlib.sha256(host_raw).hexdigest()
        result['runnerExitCode'] = args.runner_exit_code
    if args.reboot_report is not None:
        if args.reboot_report.stat().st_size > 1024 * 1024 or args.reboot_host_log.stat().st_size > 4 * 1024 * 1024:
            parser.error('Expected bounded reboot artifacts')
        after_raw = args.reboot_report.read_bytes()
        host_raw = args.reboot_host_log.read_bytes()
        after = json.loads(after_raw)
        after_result = verify(after, json.loads(raw['baseline']), args.kernel_build)
        result['guestRebootVerified'] = verify_reboot(result, after_result, json.loads(raw['report']), after, host_raw.decode('utf-8'))
        result['sha256']['rebootReport'] = hashlib.sha256(after_raw).hexdigest()
        result['sha256']['rebootHostLog'] = hashlib.sha256(host_raw).hexdigest()
    if args.clock_report is not None:
        if args.clock_report.stat().st_size > 1024 * 1024:
            parser.error('Expected bounded clock report')
        clock_raw = args.clock_report.read_bytes()
        result['guestClockAdjustmentVerified'] = verify_clock(result, json.loads(clock_raw.decode('utf-8-sig')))
        result['sha256']['clockReport'] = hashlib.sha256(clock_raw).hexdigest()
    print(json.dumps(result, indent=2))
    success = (result['verifiedCoreUsage'] and (args.shutdown_host_log is None or result['guestPowerOffVerified'])
               and (args.reboot_report is None or result['guestRebootVerified'])
               and (args.clock_report is None or result['guestClockAdjustmentVerified']))
    return 0 if success else 1


if __name__ == '__main__':
    raise SystemExit(main())
