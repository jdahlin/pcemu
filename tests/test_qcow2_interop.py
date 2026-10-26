"""Disk-light interoperability check using QEMU-generated v2/v3 images."""
import pathlib
import subprocess
import tempfile
import struct

with tempfile.TemporaryDirectory(prefix="pcemu-qcow2-interop-") as directory:
    root = pathlib.Path(directory)
    raw = root / "base.raw"
    data = bytearray(16384)
    data[0] = 42
    data[7 * 512] = 99
    data[12 * 512] = 17
    raw.write_bytes(data)
    for compat, compression in (("0.10", None), ("1.1", None), ("0.10", "zlib"), ("1.1", "zlib"), ("1.1", "zstd")):
        image = root / f"base-{compat}-{compression}.qcow2"
        options = f"compat={compat},cluster_size=4096"
        if compression and compat == "1.1": options += f",compression_type={compression}"
        command = ["qemu-img", "convert", "-f", "raw", "-O", "qcow2"]
        if compression: command += ["-c"]
        subprocess.run(command + ["-o", options, str(raw), str(image)], check=True)
        subprocess.run(["./build/qcow2-tests", str(image)], check=True)
        with subprocess.Popen(['./build/disk-tool','serve-readonly',str(image)],stdin=subprocess.PIPE,stdout=subprocess.PIPE) as reader:
            reader.stdin.write(b'R'+struct.pack('<QI',0,32));reader.stdin.flush()
            assert reader.stdout.read(1)==b'\0'
            assert reader.stdout.read(len(data))==data
            reader.stdin.write(b'Q');reader.stdin.close();assert reader.wait()==0
        # Overwrite within a compressed cluster and across a cluster boundary.
        with subprocess.Popen(['./build/disk-tool','serve',str(image)],stdin=subprocess.PIPE,stdout=subprocess.PIPE) as writer:
            patch=bytes([77])*1024
            writer.stdin.write(b'W'+struct.pack('<QI',7,2)+patch);writer.stdin.flush()
            assert writer.stdout.read(1)==b'\0'
            writer.stdin.write(b'F');writer.stdin.flush();assert writer.stdout.read(1)==b'\0'
            writer.stdin.write(b'R'+struct.pack('<QI',7,2));writer.stdin.flush()
            assert writer.stdout.read(1)==b'\0' and writer.stdout.read(1024)==patch
            writer.stdin.write(b'Q');writer.stdin.close();assert writer.wait()==0
        subprocess.run(["qemu-img", "check", str(image)], check=True)
print("QEMU v2/v3 interoperability passed")
