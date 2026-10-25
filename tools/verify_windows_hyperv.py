#!/usr/bin/env python3
"""Audit actual Windows use of the initial ARM64 Hyper-V register subset.

This does not prove GUI boot, performance benefit, SyncContext or other optional
services. Unsupported call attempts remain visible even when core use passes.
"""
import argparse
import hashlib
import json
from pathlib import Path


def verify(report, baseline, kernel_build=26100):
    errors = []
    def require(value, message):
        if not value:
            errors.append(message)
    require(report.get('schema') == 1 and report.get('mode') == 'basic', 'Expected basic-mode counters')
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
    require(type(page) is int and page > 4096 and page & 4095 == 1, 'Partition info page is not enabled')
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
                   and not (row.get('code') in (0x2, 0x13, 0x19, 0x1a) and row.get('failures') == 0)]
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
    flush_calls = [row for row in calls if row.get('code') in (0x2, 0x13)]
    flush_requests = report.get('tlbFlushRequests', 0)
    flush_mask = report.get('tlbFlushTargetMask', 0)
    flush_flags = report.get('tlbFlushFlags', 0)
    flushes = report.get('translationContextFlushes', [])
    flush_observed = (not errors and type(flush_requests) is int and flush_requests > 0
                      and counts.get(0x2, 0) + counts.get(0x13, 0) == flush_requests
                      and all(row.get('failures') == 0 for row in flush_calls)
                      and type(flush_mask) is int and 0 < flush_mask < (1 << len(vps))
                      and type(flush_flags) is int and 0 <= flush_flags <= 7
                      and type(flushes) is list and len(flushes) == len(vps)
                      and all(type(n) is int and n >= 0 for n in flushes)
                      and sum(flushes) >= flush_requests)
    return {'tlbFlushObserved': flush_observed, 'contextSyncExObserved': sync_observed and counts.get(0x1a, 0) > 0,
            'contextSyncObserved': sync_observed, 'extendedCapabilitiesObserved': query_observed, 'referenceCounterObserved': clock_observed, 'verifiedCoreUsage': not errors, 'kernelBuild': kernel_build, 'errors': errors,
            'unsupportedCalls': unsupported, 'totalFailures': report.get('failures'),
            'guiBootVerified': False, 'performanceBenefitVerified': False}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('report', type=Path)
    parser.add_argument('--baseline', type=Path, required=True)
    parser.add_argument('--kernel-build', type=int, default=26100)
    args = parser.parse_args()
    raw = {}
    for name, path in (('report', args.report), ('baseline', args.baseline)):
        if path.stat().st_size > 1024 * 1024:
            parser.error('Expected bounded counter snapshots')
        raw[name] = path.read_bytes()
    result = verify(json.loads(raw['report']), json.loads(raw['baseline']), args.kernel_build)
    result['sha256'] = {name: hashlib.sha256(data).hexdigest() for name, data in raw.items()}
    print(json.dumps(result, indent=2))
    return 0 if result['verifiedCoreUsage'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
