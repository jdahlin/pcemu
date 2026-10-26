#!/usr/bin/env python3
"""Measure an existing macOS HVF process; 100% CPU means one logical CPU.

SIGUSR1 requests the runner's existing live counters. Capture work happens
outside each timed CPU interval. Leave the VM at an idle desktop first.
"""
import argparse
import ctypes
import json
import os
from pathlib import Path
import re
import signal
import statistics
import time


class Timebase(ctypes.Structure):
    _fields_ = [('numer', ctypes.c_uint32), ('denom', ctypes.c_uint32)]


class Usage(ctypes.Structure):
    # rusage_info_v0, from the macOS SDK's sys/resource.h.
    _fields_ = [('uuid', ctypes.c_uint8 * 16)] + [
        (name, ctypes.c_uint64) for name in (
            'user', 'system', 'packageWakeups', 'interruptWakeups', 'pageins',
            'wired', 'resident', 'footprint', 'start', 'exit')]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('pid', type=int)
    parser.add_argument('--log', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--settle', type=float, default=15)
    parser.add_argument('--duration', type=float, default=20)
    parser.add_argument('--count', type=int, default=3)
    args = parser.parse_args()
    if args.duration <= 0 or args.count <= 0 or args.settle < 0:
        parser.error('duration/count must be positive and settle nonnegative')
    lib = ctypes.CDLL('/usr/lib/libproc.dylib', use_errno=True)
    system = ctypes.CDLL('/usr/lib/libSystem.B.dylib')
    base = Timebase()
    if system.mach_timebase_info(ctypes.byref(base)):
        raise RuntimeError('mach_timebase_info failed')
    nanoseconds_per_tick = base.numer / base.denom
    lib.proc_pid_rusage.argtypes = [ctypes.c_int, ctypes.c_int, ctypes.c_void_p]
    lib.proc_pid_rusage.restype = ctypes.c_int

    def usage():
        value = Usage()
        if lib.proc_pid_rusage(args.pid, 0, ctypes.byref(value)):
            raise OSError(ctypes.get_errno(), 'proc_pid_rusage')
        return {name: getattr(value, name) for name in (
            'user', 'system', 'packageWakeups', 'interruptWakeups', 'pageins', 'footprint')}

    def capture():
        offset = args.log.stat().st_size
        os.kill(args.pid, signal.SIGUSR1)
        deadline = time.monotonic() + 5
        while time.monotonic() < deadline:
            with args.log.open('rb') as stream:
                stream.seek(offset)
                rows = stream.read().decode(errors='replace').splitlines(keepends=True)
            rows = [row for row in rows if row.startswith('UEFI live capture:') and row.endswith('\n')]
            if rows:
                return {key: int(value) for key, value in re.findall(
                    r'\b(exits|diskRequests|netTx|netRx|reports|keys|pointers)=(\d+)', rows[-1])}
            time.sleep(0.05)
        raise RuntimeError('No fresh UEFI live capture; check PID/log and --run-forever')

    result = {
        'pid': args.pid, 'sampleSeconds': args.duration, 'settleSeconds': args.settle,
        'percentConvention': '100% = one logical CPU',
        'machTimebase': {'numer': base.numer, 'denom': base.denom}, 'samples': []}
    time.sleep(args.settle)
    for _ in range(args.count):
        counters_before = capture()
        before = usage()
        started = time.monotonic()
        time.sleep(args.duration)
        elapsed = time.monotonic() - started
        after = usage()
        counters_after = capture()
        user = (after['user'] - before['user']) * nanoseconds_per_tick / 1e9
        kernel = (after['system'] - before['system']) * nanoseconds_per_tick / 1e9
        row = {'wallSeconds': elapsed, 'cpuPercent': 100 * (user + kernel) / elapsed,
               'userCpuSeconds': user, 'systemCpuSeconds': kernel,
               'footprintBytes': after['footprint']}
        for key in ('packageWakeups', 'interruptWakeups', 'pageins'):
            row[key + 'PerSecond'] = (after[key] - before[key]) / elapsed
        for key in counters_after.keys() & counters_before.keys():
            row[key + 'PerSecond'] = (counters_after[key] - counters_before[key]) / elapsed
        result['samples'].append(row)
        result['medianCpuPercent'] = statistics.median(r['cpuPercent'] for r in result['samples'])
        args.output.write_text(json.dumps(result, indent=2) + '\n')
        print(json.dumps(row), flush=True)


if __name__ == '__main__':
    main()
