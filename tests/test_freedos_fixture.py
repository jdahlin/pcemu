"""Offline startup and path checks on an independently constructed FAT16 volume."""
import io
from pathlib import Path
import struct
import sys
import unittest
from unittest.mock import patch
import tempfile
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'tools'))
from fat_stage import Writer
import freedos_fixture
from freedos_fixture import put_root, read_path, startup


def volume():
    data=bytearray(8*1024*1024)
    data[3:11]=b'MSDOS5.0'
    struct.pack_into('<HBHBHHBH',data,11,512,1,1,2,512,16384,0xf8,64)
    data[510:512]=b'\x55\xaa'
    for sector in [1,65]:struct.pack_into('<HH',data,sector*512,0xfff8,0xffff)
    return io.BytesIO(data)


class Startup(unittest.TestCase):
    def test_altered_download_is_rejected_before_extraction(self):
        with tempfile.TemporaryDirectory() as path:
            root=Path(path);cache=root/'build/freedos';cache.mkdir(parents=True)
            (cache/'FD14-LiteUSB.zip').write_bytes(b'altered archive')
            with patch.object(freedos_fixture,'ROOT',root), patch.object(freedos_fixture.subprocess,'run') as run:
                with self.assertRaisesRegex(ValueError,'checksum mismatch'):
                    freedos_fixture.prepare(root/'fixture.qcow2')
                run.assert_not_called()

    def test_freedos_preferred_and_standard_startup_files_agree(self):
        stream=volume();fat=Writer(stream);put_root(fat,'KERNEL.SYS',b'kernel fixture')
        startup(fat,b'FILES=40\r\n',b'ECHO FIRST\r\n')
        startup(fat,b'BUFFERS=20\r\n',b'ECHO SECOND\r\n')
        stream.seek(0);fat=Writer(stream)
        config=read_path(fat,'CONFIG.SYS')
        self.assertEqual(config,read_path(fat,'FDCONFIG.SYS'))
        self.assertIn(b'/P=C:\\AUTOEXEC.BAT',config)
        self.assertEqual(read_path(fat,'AUTOEXEC.BAT'),b'ECHO SECOND\r\n')
        self.assertEqual(read_path(fat,'FDAUTO.BAT'),b'ECHO SECOND\r\n')
        names=[e[1] for e in fat.entries()]
        self.assertEqual(len(names),len(set(names)))

    def test_ordinary_dos_does_not_get_a_freedos_shell(self):
        fat=Writer(volume());startup(fat,b'FILES=30\r\n',b'ECHO DOS\r\n')
        self.assertEqual(read_path(fat,'CONFIG.SYS'),b'FILES=30\r\n')
        with self.assertRaises(FileNotFoundError):read_path(fat,'FDCONFIG.SYS')

    def test_explicit_directory_reads_and_missing_paths(self):
        fat=Writer(volume());fat.put_directory('DRIVERS',{'PROBE.COM':bytes(range(256))*3})
        self.assertEqual(read_path(fat,'DRIVERS/PROBE.COM'),bytes(range(256))*3)
        for path in ['DRIVERS/MISSING.COM','MISSING/PROBE.COM']:
            with self.assertRaises(FileNotFoundError):read_path(fat,path)
        with self.assertRaises(ValueError):read_path(fat,'DRIVERS/PROBE.COM/FILE')


if __name__=='__main__':unittest.main()
