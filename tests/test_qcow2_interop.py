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
    for compat in ("0.10", "1.1"):
        image = root / f"base-{compat}.qcow2"
        subprocess.run(["qemu-img", "convert", "-f", "raw", "-O", "qcow2",
                        "-o", f"compat={compat},cluster_size=4096", str(raw), str(image)], check=True)
        subprocess.run(["./build/qcow2-tests", str(image)], check=True)
        subprocess.run(["qemu-img", "check", str(image)], check=True)
print("QEMU v2/v3 interoperability passed")
