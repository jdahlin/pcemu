#!/usr/bin/env python3
"""Flatten any PCEMU disk into qcow2 using a private read-only NBD socket.

Stop guests using the source first. No RAW staging file is created. Originals
are retained; output is published only after qemu-img check and byte comparison.
"""
import argparse
import fcntl
import os
from pathlib import Path
import socket
import shutil
import struct
import subprocess
import tempfile
import threading


def exact(stream, length):
    result = bytearray()
    while len(result) < length:
        part = stream.read(length - len(result))
        if not part:
            raise EOFError('Short disk/NBD transfer')
        result.extend(part)
    return bytes(result)


def convert(source, destination, size, compression="zstd"):
    if size <= 0 or size % 512:
        raise ValueError('Size must be a positive multiple of 512')
    if destination.exists():
        raise FileExistsError(destination)
    executable = Path(__file__).resolve().parent.parent / 'build/disk-tool'
    actual = int(subprocess.check_output([str(executable), 'size', str(source)]))
    if size != actual:
        raise ValueError(f'Source size is {actual}, requested {size}')
    partial = destination.with_name(destination.name + '.partial')
    # Exclusive creation also protects against accidentally reusing old output.
    with partial.open('xb'):
        pass
    errors = []
    try:
        with tempfile.TemporaryDirectory(prefix='pcemu-nbd-') as directory:
            address = str(Path(directory) / 'disk.sock')
            with socket.socket(socket.AF_UNIX) as listener:
                listener.bind(address)
                listener.listen()
                stopping = threading.Event()

                def serve():
                    listener.settimeout(0.5)
                    while not stopping.is_set():
                        try:
                            connection, _ = listener.accept()
                        except socket.timeout:
                            continue
                        try:
                            with connection, subprocess.Popen(
                                [str(executable), 'serve-readonly', str(source)],
                                stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                            ) as disk:
                                try:
                                    with connection.makefile('rwb', buffering=0) as wire:
                                        # Oldstyle NBD: fixed export, read-only, no negotiation.
                                        connection.sendall(struct.pack('>QQQI', 0x4e42444d41474943,
                                                              0x420281861253, size, 3) + bytes(124))
                                        while True:
                                            magic, flags, command, handle, offset, length = struct.unpack(
                                                '>IHHQQI', exact(wire, 28))
                                            if magic != 0x25609513:
                                                raise ValueError('Invalid NBD request')
                                            if command == 2:
                                                break
                                            if command != 0 or flags or length > 32 * 1024 * 1024 or offset % 512 or length % 512 or offset > size or length > size - offset:
                                                raise ValueError('Invalid read-only NBD request')
                                            output = bytearray()
                                            while length:
                                                count = min(length // 512, 2048)
                                                disk.stdin.write(b'R' + struct.pack('<QI', offset // 512, count))
                                                disk.stdin.flush()
                                                if exact(disk.stdout, 1) != b'\0':
                                                    raise OSError('Disk read failed')
                                                output.extend(exact(disk.stdout, count * 512))
                                                offset += count * 512
                                                length -= count * 512
                                            connection.sendall(struct.pack('>IIQ', 0x67446698, 0, handle) + output)
                                finally:
                                    disk.stdin.write(b'Q')
                                    disk.stdin.flush()
                                    disk.stdin.close()
                                    if disk.wait(timeout=30):
                                        raise OSError('Disk reader failed')
                        except Exception as error:
                            errors.append(error)
                            break

                worker = threading.Thread(target=serve, daemon=True)
                worker.start()
                url = 'nbd:unix:' + address
                try:
                    # Create only empty metadata locally: NAS file locking may be
                    # unsupported, while the actual guest data stays at destination.
                    empty = Path(directory) / 'empty.qcow2'
                    subprocess.run(['qemu-img', 'create', '-q', '-f', 'qcow2',
                                    '-o', 'compat=1.1,cluster_size=65536,compression_type='+compression, str(empty), str(size)], check=True)
                    shutil.copyfile(empty, partial)
                    target = ('driver=qcow2,file.driver=file,file.filename=' +
                              str(partial).replace(',', ',,') + ',file.locking=off')
                    subprocess.run(['qemu-img', 'convert', '-p', '-c', '-n', '-f', 'raw',
                                    '--target-image-opts', url, target], check=True)
                    subprocess.run(['qemu-img', 'check', '--image-opts', target], check=True)
                    subprocess.run(['qemu-img', 'compare', '-p', '--image-opts',
                                    'driver=raw,file.driver=nbd,file.server.type=unix,file.server.path=' + address,
                                    target], check=True)
                finally:
                    stopping.set()
                    worker.join(timeout=60)
                if worker.is_alive():
                    raise RuntimeError('Disk reader did not stop')
                if errors:
                    raise errors[0]
        # Reserve the destination exclusively, then atomically replace our own
        # empty reservation. SMB supports this even when hard links and macOS
        # exclusive rename are unavailable. Existing destinations are untouched.
        with destination.open('xb'):
            pass
        try:
            os.replace(partial, destination)
        except BaseException:
            destination.unlink()
            raise
    except BaseException:
        partial.unlink(missing_ok=True)
        raise


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('source', type=Path)
    parser.add_argument('destination', type=Path)
    parser.add_argument('--size', type=int, required=True, help='Guest-visible size in bytes')
    args = parser.parse_args()
    with Path(str(args.source.resolve()) + '.lock').open('a+b') as lock:
        fcntl.flock(lock, fcntl.LOCK_SH | fcntl.LOCK_NB)
        convert(args.source.resolve(), args.destination.resolve(), args.size)
