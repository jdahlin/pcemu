"""Read cumulative CPU time of a live macOS process without sampling it.

ABI: sys/resource.h rusage_info_v0 and libproc.h proc_pid_rusage (macOS 10.9+).
User/system counters are Mach absolute time and require mach_timebase_info.
start_abstime is retained as an opaque process identity.
"""
import ctypes
from dataclasses import dataclass
from functools import lru_cache
import os
import sys


class _RusageV0(ctypes.Structure):
    _fields_ = [('uuid', ctypes.c_uint8 * 16)] + [
        (name, ctypes.c_uint64) for name in (
            'user_time', 'system_time', 'pkg_idle_wkups', 'interrupt_wkups',
            'pageins', 'wired_size', 'resident_size', 'phys_footprint',
            'proc_start_abstime', 'proc_exit_abstime')]


@dataclass(frozen=True)
class CpuSnapshot:
    pid: int
    start_abstime: int
    user_ns: int
    system_ns: int


@lru_cache(maxsize=1)
def _reader():
    library = ctypes.CDLL('/usr/lib/libproc.dylib', use_errno=True)
    function = library.proc_pid_rusage
    function.argtypes = [ctypes.c_int, ctypes.c_int, ctypes.c_void_p]
    function.restype = ctypes.c_int
    return function


@lru_cache(maxsize=1)
def mach_timebase():
    class Timebase(ctypes.Structure):
        _fields_ = [('numer', ctypes.c_uint32), ('denom', ctypes.c_uint32)]
    value = Timebase()
    function = ctypes.CDLL('/usr/lib/libSystem.B.dylib').mach_timebase_info
    function.argtypes = [ctypes.POINTER(Timebase)]
    function.restype = ctypes.c_int
    if function(ctypes.byref(value)) != 0 or not value.numer or not value.denom:
        raise OSError('Cannot determine Mach CPU counter timebase')
    return value.numer, value.denom


def read_process_cpu(pid):
    if sys.platform != 'darwin':
        raise NotImplementedError('Process CPU timing requires macOS proc_pid_rusage')
    if pid <= 0:
        raise ValueError('A positive process ID is required')
    value = _RusageV0()
    if _reader()(pid, 0, ctypes.byref(value)) != 0:
        error = ctypes.get_errno()
        raise OSError(error, os.strerror(error))
    numer, denom = mach_timebase()
    return CpuSnapshot(pid, value.proc_start_abstime,
                       value.user_time * numer // denom, value.system_time * numer // denom)


def cpu_interval(before, after):
    if (before.pid, before.start_abstime) != (after.pid, after.start_abstime):
        raise ValueError('Process identity changed during CPU interval')
    user = after.user_ns - before.user_ns
    system = after.system_ns - before.system_ns
    if user < 0 or system < 0:
        raise ValueError('Process CPU counters moved backwards')
    return {'status': 'available', 'source': 'macOS proc_pid_rusage RUSAGE_INFO_V0; mach_timebase_info conversion',
            'userMs': user / 1e6, 'systemMs': system / 1e6,
            'totalMs': (user + system) / 1e6}
