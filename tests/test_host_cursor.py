"""Execute the actual Win16 cursor DDI headless with a 512-byte RAM-only guest."""
import json
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]

@unittest.skipUnless(shutil.which('nasm') and (ROOT/'build/pcemu').exists(), 'build pcemu and install nasm')
class HostCursorGuestTest(unittest.TestCase):
    def run_guest(self, hide=False):
        with tempfile.TemporaryDirectory() as directory:
            work = Path(directory)
            source = '''bits 16
org 0x7c00
xor ax, ax
mov ds, ax
mov ss, ax
mov sp, 0x7c00
mov bx, 0x1234
mov si, 0x5678
push word 0
push word shape
call 0:set_cursor
cmp bx, 0x1234
jne failed
cmp si, 0x5678
jne failed
cmp ax, 1
jne failed
'''
            if hide:
                source += 'push word 0\npush word 0\ncall 0:set_cursor\n'
            source += '''push word 20
push word 30
call 0:move_cursor
call 0:check_cursor
mov al, 'P'
out 0xe9, al
hlt
failed:
mov al, 'F'
out 0xe9, al
hlt
shape:
dw 2,3,8,8,1
db 1,1
times 8 db 0
 db 0x80,0x40,0x20,0x10,8,4,2,1
%include "guest/host_cursor.asm"
times 510-($-$$) db 0
dw 0xaa55
'''
            (work/'boot.asm').write_text(source)
            subprocess.run(['nasm','-f','bin',str(work/'boot.asm'),'-o',str(work/'boot.img')], cwd=ROOT, check=True)
            result = subprocess.run([str(ROOT/'build/pcemu'),'--headless','--floppy',str(work/'boot.img'),
                                     '--boot-floppy','--steps','10000','--no-network',
                                     '--state-json',str(work/'state.json')], capture_output=True, text=True, check=True)
            self.assertEqual(result.stdout.split('PCEmu BIOS', 1)[0], 'P')
            return json.loads((work/'state.json').read_text())['hostCursor']

    def test_shape_upload_uses_real_guest_out_instructions(self):
        cursor = self.run_guest()
        self.assertEqual((cursor['active'],cursor['visible'],cursor['size']), (1,1,28))

    def test_windows_hide_cursor(self):
        cursor = self.run_guest(hide=True)
        self.assertEqual((cursor['active'],cursor['visible']), (1,0))

if __name__ == '__main__':
    unittest.main()
