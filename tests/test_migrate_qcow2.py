"""Real legacy-format recovery, conversion, and dependency safety with tiny disks."""
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest

ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'tools'))
import migrate_qcow2 as migration

def fnv(data):
    value=14695981039346656037
    for b in data: value=((value^b)*1099511628211)&((1<<64)-1)
    return value

def legacy(path,parent,data):
    name=str(parent.resolve()).encode()
    header=b'PCEMUCOW'+struct.pack('<IIQI',1,4096,32,len(name))+name+struct.pack('<Q',fnv(name))
    packed=subprocess.check_output(['zstd','-q','-1','-c'],input=data)
    path.write_bytes(header+struct.pack('<QIIQ',1,len(packed),4096,fnv(data)^1)+packed)

class MigrationTest(unittest.TestCase):
    def test_real_legacy_bytes_survive_compressed_conversion(self):
        with tempfile.TemporaryDirectory() as d:
            w=Path(d);base=w/'base.raw';base.write_bytes(bytes(16384))
            source=w/'old.pcd';data=bytes(range(256))*16;legacy(source,base,data)
            result=migration.migrate([source],remove=True)
            self.assertFalse(source.exists())
            output=w/'old.qcow2';self.assertTrue(result[0]['byteComparisonPassed'])
            raw=w/'check.raw'
            subprocess.run(['qemu-img','convert','-O','raw',str(output),str(raw)],check=True)
            self.assertEqual(raw.read_bytes(),bytes(4096)+data+bytes(8192))
            info=subprocess.check_output(['qemu-img','info','--output=json',str(output)],text=True)
            self.assertIn('zstd',info)

    def test_destination_and_unlisted_dependents_are_preserved(self):
        with tempfile.TemporaryDirectory() as d:
            w=Path(d);(w/'media').mkdir();base=w/'media/base.raw';base.write_bytes(bytes(16384))
            source=w/'media/old.pcd';legacy(source,base,bytes(4096))
            child=w/'media/child.pcd';legacy(child,source,bytes(4096))
            original=migration.ROOT;migration.ROOT=w
            try:
                with self.assertRaisesRegex(RuntimeError,'Unlisted dependent'):
                    migration.migrate([source],remove=True)
            finally:migration.ROOT=original
            self.assertTrue(source.exists());self.assertTrue(child.exists())
            output=source.with_suffix('.qcow2');output.write_bytes(b'preserve me')
            with self.assertRaises(FileExistsError):migration.migrate([source])
            self.assertEqual(output.read_bytes(),b'preserve me')

if __name__=='__main__':unittest.main()
