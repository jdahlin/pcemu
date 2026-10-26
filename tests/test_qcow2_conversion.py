"""Exercise flattening a native overlay and non-overwrite guarantees."""
import pathlib
import subprocess
import tempfile

with tempfile.TemporaryDirectory(prefix='pcemu-convert-test-') as directory:
    root = pathlib.Path(directory)
    raw = root / 'base.raw'
    raw.write_bytes(bytes(16384))
    overlay = root / 'disk.qcow2'
    subprocess.run(['./build/disk-tool', 'overlay', str(overlay), str(raw)], check=True)
    import struct
    writer = subprocess.Popen(['./build/disk-tool', 'serve', str(overlay)], stdin=subprocess.PIPE, stdout=subprocess.PIPE)
    writer.stdin.write(b'W' + struct.pack('<QI', 0, 1) + b'*' + bytes(511))
    writer.stdin.flush()
    assert writer.stdout.read(1) == b'\0'
    writer.stdin.write(b'W' + struct.pack('<QI', 7, 1) + b'c' + bytes(511))
    writer.stdin.flush()
    assert writer.stdout.read(1) == b'\0'
    writer.stdin.write(b'Q')
    writer.stdin.close()
    assert writer.wait() == 0
    original = overlay.read_bytes()
    output = root / 'result, NAS.qcow2'
    command = ['python3', 'tools/convert_qcow2.py', str(overlay), str(output), '--size', '16384']
    subprocess.run(command, check=True)
    subprocess.run(['./build/qcow2-tests', str(output)], check=True)
    assert overlay.read_bytes() == original
    assert subprocess.run(command, capture_output=True).returncode != 0
    assert not output.with_name(output.name + '.partial').exists()
print('QCOW2 overlay conversion passed')
