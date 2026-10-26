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


class ThreadUsage(ctypes.Structure):
    # proc_threadinfo, from sys/proc_info.h. Unlike rusage_info_v0,
    # these per-thread times are already nanoseconds.
    _fields_ = [('user', ctypes.c_uint64), ('system', ctypes.c_uint64)] + [
        (name, ctypes.c_int32) for name in (
            'usage', 'policy', 'state', 'flags', 'sleep', 'curpri', 'priority', 'maxpri')
    ] + [('name', ctypes.c_char * 64)]


def thread_usage(lib, pid):
    handles = (ctypes.c_uint64 * 1024)()
    size = lib.proc_pidinfo(pid, 6, 0, handles, ctypes.sizeof(handles))
    if size <= 0 or size >= ctypes.sizeof(handles):
        raise RuntimeError('Cannot enumerate process threads (or buffer exhausted)')
    result = {}
    for handle in list(handles)[:size // 8]:
        if not handle:
            continue
        value = ThreadUsage()
        received = lib.proc_pidinfo(pid, 5, handle, ctypes.byref(value), ctypes.sizeof(value))
        if received == ctypes.sizeof(value):
            result[handle] = {'user': value.user, 'system': value.system,
                              'name': value.name.decode(errors='replace')}
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('pid', type=int)
    parser.add_argument('--log', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--settle', type=float, default=15)
    parser.add_argument('--duration', type=float, default=20)
    parser.add_argument('--count', type=int, default=3)
    parser.add_argument('--threads', action='store_true', help='include per-thread CPU deltas')
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
    lib.proc_pidinfo.argtypes = [ctypes.c_int, ctypes.c_int, ctypes.c_uint64,
                                ctypes.c_void_p, ctypes.c_int]
    lib.proc_pidinfo.restype = ctypes.c_int

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
        threads_before = thread_usage(lib, args.pid) if args.threads else {}
        before = usage()
        started = time.monotonic()
        time.sleep(args.duration)
        elapsed = time.monotonic() - started
        after = usage()
        threads_after = thread_usage(lib, args.pid) if args.threads else {}
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
        if args.threads:
            row['threads'] = []
            for handle in threads_before.keys() & threads_after.keys():
                first, last = threads_before[handle], threads_after[handle]
                user_seconds = (last['user'] - first['user']) / 1e9
                system_seconds = (last['system'] - first['system']) / 1e9
                row['threads'].append({
                    'handle': hex(handle), 'name': last['name'],
                    'userCpuSeconds': user_seconds, 'systemCpuSeconds': system_seconds,
                    'cpuPercent': 100 * (user_seconds + system_seconds) / elapsed})
            row['threads'].sort(key=lambda t: t['cpuPercent'], reverse=True)
            row['threadCpuSumPercent'] = sum(t['cpuPercent'] for t in row['threads'])
            row['threadCoverageNote'] = 'Only threads present at both endpoints; snapshots are sequential'
        result['samples'].append(row)
        result['medianCpuPercent'] = statistics.median(r['cpuPercent'] for r in result['samples'])
        args.output.write_text(json.dumps(result, indent=2) + '\n')
        print(json.dumps(row), flush=True)


if __name__ == '__main__':
    main()
