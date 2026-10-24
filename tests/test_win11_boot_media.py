"""Original synthetic fixtures for Windows Setup boot-media staging."""
import io
from pathlib import Path
import struct
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
try:
    import prepare_win11_boot as boot
except ImportError:
    boot = None


@unittest.skipIf(boot is None, 'Optional pycdlib media tooling is not installed')
class Win11BootMedia(unittest.TestCase):
    def make_iso(self, path, machine=0xaa64, subsystem=10):
        pe = bytearray(512); pe[:2] = b'MZ'; struct.pack_into('<I', pe, 60, 128)
        pe[128:132] = b'PE\0\0'; struct.pack_into('<H', pe, 132, machine)
        struct.pack_into('<H', pe, 128 + 92, subsystem)
        xml = '<WIM><IMAGE INDEX="2"><NAME>Synthetic Setup</NAME><WINDOWS><ARCH>12</ARCH><VERSION><BUILD>26100</BUILD></VERSION></WINDOWS></IMAGE></WIM>'.encode('utf-16')
        wim = bytearray(208); wim[:8] = b'MSWIM\0\0\0'
        struct.pack_into('<QQ', wim, 72, len(xml), 208)
        self.wim = bytes(wim) + xml
        iso = boot.pycdlib.PyCdlib(); iso.new(udf='2.60', interchange_level=3)
        for name in ('/EFI', '/EFI/BOOT', '/EFI/MICROSOFT', '/EFI/MICROSOFT/BOOT', '/BOOT', '/SOURCES'):
            iso.add_directory(iso_path=name, udf_path=name.lower())
        for name, data in (('/EFI/BOOT/BOOTAA64.EFI', bytes(pe)), ('/EFI/MICROSOFT/BOOT/BCD', b'regf-fixture'),
                           ('/BOOT/BOOT.SDI', b'synthetic-sdi'), ('/SOURCES/BOOT.WIM', self.wim)):
            iso.add_fp(io.BytesIO(data), len(data), iso_path=name + ';1', udf_path=name.lower())
        iso.write(str(path)); iso.close()

    def test_boot_files_and_unchanged_wim(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp); source = root / 'fixture.iso'; target = root / 'boot.raw'
            self.make_iso(source)
            report = boot.prepare(source, target)
            self.assertFalse(report['installImageIncluded'])
            self.assertEqual(len(report['files']), 4)
            fat = boot.Writer(target)
            try:
                def entry(cluster, name):
                    if cluster == 0:
                        pos, count = fat.rootbase, fat.nroot * 32
                    else:
                        pos, count = fat.database + (cluster - 2) * fat.cluster_bytes, fat.cluster_bytes
                    fat.f.seek(pos); data = fat.f.read(count)
                    for offset in range(0, count, 32):
                        item = data[offset:offset + 32]
                        if item[:11] == name:
                            return struct.unpack_from('<H', item, 26)[0], struct.unpack_from('<I', item, 28)[0]
                    self.fail('Missing FAT entry ' + repr(name))
                sources, _ = entry(0, b'SOURCES    ')
                cluster, size = entry(sources, b'BOOT    WIM')
                self.assertEqual(fat.read_file(cluster, size), self.wim)
                efi, _ = entry(0, b'EFI        ')
                microsoft, _ = entry(efi, b'MICROS~1   ')
                directory, _ = entry(microsoft, b'BOOT       ')
                cluster, size = entry(directory, b'BCD        ')
                self.assertEqual(fat.read_file(cluster, size), b'regf-fixture')
            finally:
                fat.f.close()

    def test_microsoft_long_directory_name(self):
        entry = boot.directory_entry('MICROSOFT', 4)
        self.assertEqual(len(entry), 64)
        self.assertEqual(entry[0], 0x41); self.assertEqual(entry[11], 15)
        text = entry[1:11] + entry[14:26] + entry[28:32]
        self.assertEqual(text.decode('utf-16le').split('\0')[0], 'MICROSOFT')
        checksum = 0
        for byte in entry[32:43]:
            checksum = (((checksum & 1) << 7) + (checksum >> 1) + byte) & 255
        self.assertEqual(entry[13], checksum)

    def test_wrong_architecture_preserves_destination(self):
        with tempfile.TemporaryDirectory() as tmp:
            source, target = Path(tmp) / 'fixture.iso', Path(tmp) / 'boot.raw'
            self.make_iso(source, 0x8664)
            with self.assertRaisesRegex(ValueError, 'not ARM64'):
                boot.prepare(source, target)
            self.assertFalse(target.exists())

    def test_windows_boot_application_is_not_efi(self):
        with tempfile.TemporaryDirectory() as tmp:
            source, target = Path(tmp) / 'fixture.iso', Path(tmp) / 'boot.raw'
            self.make_iso(source, subsystem=16)
            with self.assertRaisesRegex(ValueError, 'not an EFI application'):
                boot.prepare(source, target)
            self.assertFalse(target.exists())

    def test_truncated_payload_and_overwrite_rejected(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / 'boot.raw'; boot.format_disk(path)
            with self.assertRaises(FileExistsError):
                boot.format_disk(path)
            fat = boot.Writer(path)
            try:
                with self.assertRaisesRegex(ValueError, 'Truncated'):
                    boot.allocate_stream(fat, io.BytesIO(b'short'), 32)
            finally:
                fat.f.close()


if __name__ == '__main__':
    unittest.main()
