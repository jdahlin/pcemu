"""Disk-light interoperability check using QEMU-generated v2/v3 images."""
import pathlib
import subprocess
import tempfile

with tempfile.TemporaryDirectory(prefix="pcemu-qcow2-interop-") as directory:
    root = pathlib.Path(directory)
    raw = root / "base.raw"
    data = bytearray(16384)
    data[0] = 42
    data[7 * 512] = 99
    raw.write_bytes(data)
    for compat, compression in (("0.10", None), ("1.1", None), ("0.10", "zlib"), ("1.1", "zlib"), ("1.1", "zstd")):
        image = root / f"base-{compat}-{compression}.qcow2"
        options = f"compat={compat},cluster_size=4096"
        if compression and compat == "1.1": options += f",compression_type={compression}"
        command = ["qemu-img", "convert", "-f", "raw", "-O", "qcow2"]
        if compression: command += ["-c"]
        subprocess.run(command + ["-o", options, str(raw), str(image)], check=True)
        subprocess.run(["./build/qcow2-tests", str(image)], check=True)
        subprocess.run(["qemu-img", "check", str(image)], check=True)
print("QEMU v2/v3 interoperability passed")
