"""Offline, seekable access to PCEMU compressed disks without a raw staging copy.

The disk must not be attached to a running emulator. Use a private COW child.
"""
import io
import struct
import subprocess
from pathlib import Path

class CowFile:
    def __init__(self, path):
        executable = Path(__file__).resolve().parent.parent / 'build/disk-tool'
        self.process = subprocess.Popen([str(executable), 'serve', str(path)], stdin=subprocess.PIPE, stdout=subprocess.PIPE)
        self.position = 0
        self.closed = False

    def _exact(self, size):
        data = bytearray()
        while len(data) < size:
            chunk = self.process.stdout.read(size - len(data))
            if not chunk:
                raise OSError('disk-tool closed its pipe')
            data.extend(chunk)
        return bytes(data)

    def _transfer(self, op, sector, count, data=b''):
        self.process.stdin.write(op + struct.pack('<QI', sector, count) + data)
        self.process.stdin.flush()
        if self._exact(1) != b'\0':
            raise OSError('disk-tool failed')
        return self._exact(count * 512) if op == b'R' else b''

    def seek(self, offset, whence=io.SEEK_SET):
        if whence == io.SEEK_CUR:
            offset += self.position
        elif whence != io.SEEK_SET:
            raise ValueError('Only absolute/current seeks supported')
        if offset < 0:
            raise ValueError('Negative disk position')
        self.position = offset
        return offset

    def tell(self):
        return self.position

    def read(self, size):
        if size < 0:
            raise ValueError("Read size must be nonnegative")
        result = bytearray()
        while size:
            sector, skip = divmod(self.position, 512)
            amount = min(size, 1024*1024-skip)
            count = (skip+amount+511)//512
            result.extend(self._transfer(b'R', sector, count)[skip:skip+amount])
            self.position += amount
            size -= amount
        return bytes(result)

    def write(self, data):
        view = memoryview(data)
        written = 0
        while written < len(view):
            sector, skip = divmod(self.position, 512)
            amount = min(len(view)-written, 1024*1024-skip)
            count = (skip+amount+511)//512
            block = bytearray(self._transfer(b'R', sector, count)) if skip or amount % 512 else bytearray(count*512)
            block[skip:skip+amount] = view[written:written+amount]
            self._transfer(b'W', sector, count, block)
            self.position += amount
            written += amount
        return written

    def flush(self):
        self.process.stdin.write(b'F')
        self.process.stdin.flush()
        self._exact(1)

    def close(self):
        if self.closed:
            return
        self.process.stdin.write(b'Q')
        self.process.stdin.flush()
        self.process.stdin.close()
        self.process.stdout.close()
        code = self.process.wait(timeout=15)
        self.closed = True
        if code:
            raise OSError(f'disk-tool failed ({code})')

    def __enter__(self):
        return self

    def __exit__(self, *_):
        self.close()
