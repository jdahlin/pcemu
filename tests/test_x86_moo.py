"""Independent parser boundaries and negative checks for the hardware oracle."""
import io
import json
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'tools'))
from x86_moo import chunks,read_moo,state,write_vectors,REGS
from test_x86_hardware import flag_mask,undefined_sib,shift_contract
ROOT=Path(__file__).resolve().parents[1]


def chunk(tag,payload):return tag+struct.pack('<I',len(payload))+payload


class Moo(unittest.TestCase):
    def test_chunk_bounds(self):
        self.assertEqual([(t,bytes(p)) for t,p in chunks(chunk(b'TEST',b'ab'))],[(b'TEST',b'ab')])
        for data in [b'x',chunk(b'TEST',b'ab')[:-1],b'TEST'+struct.pack('<I',0xffffffff)]:
            with self.assertRaises(ValueError):list(chunks(data))

    def test_register_and_memory_layout(self):
        data=chunk(b'RG32',struct.pack('<III',0b101,0x12345678,0xabcdef01))
        data+=chunk(b'RAM ',struct.pack('<IIBIB',2,0x123,0xff,0x100234,0x80))
        self.assertEqual(state(data),{'regs':{'cr0':0x12345678,'eax':0xabcdef01},'ram':[[0x123,255],[0x100234,128]]})
        for payload in [struct.pack('<I',1<<24),struct.pack('<I',3),b'x']:
            with self.assertRaises(ValueError):state(chunk(b'RG32',payload))

    def test_header_count_and_hash_validation(self):
        header=bytes([1,0,0,0])+struct.pack('<I',1)+b'386E'
        with self.assertRaisesRegex(ValueError,'count mismatch'):read_moo(chunk(b'MOO ',header))
        with self.assertRaises(ValueError):read_moo(chunk(b'MOO ',b'bad'))
        test=struct.pack('<I',0)+chunk(b'HASH',b'bad')
        with self.assertRaisesRegex(ValueError,'hash'):read_moo(chunk(b'MOO ',header)+chunk(b'TEST',test))

    def test_defined_flag_mask_and_documented_sib_quirk(self):
        rows=[{'op':'F6','ex':'4','f_umask':'0xFF2B'},{'op':'01','ex':'','f_umask':''}]
        mask=flag_mask('66F6.4',rows)
        self.assertTrue(mask&1 and mask&0x800)
        self.assertEqual(mask&(4|16|64|128),0)
        self.assertEqual(flag_mask('676601',rows),0x37fd7)
        self.assertTrue(undefined_sib({'bytes':[0x67,0x01,0x04,0x60]}))
        self.assertFalse(undefined_sib({'bytes':[0x67,0x01,0x04,0x20]}))
        self.assertFalse(undefined_sib({'bytes':[0x01,0x04,0x60]}))

    def test_shift_defined_state_contract(self):
        make=lambda code,cl=0:{'bytes':code+[0xf4],'initial':{'regs':{'ecx':cl}}}
        self.assertEqual(shift_contract(make([0xd3,0xe0],0),0x37fc7),(0x37fd7,False))
        self.assertTrue(shift_contract(make([0xd3,0xe0],1),0x37fc7)[0]&0x800)
        self.assertFalse(shift_contract(make([0xd3,0xe0],2),0x37fc7)[0]&0x800)
        self.assertFalse(shift_contract(make([0xd2,0xe0],8),0x37fc7)[0]&1)
        self.assertTrue(shift_contract(make([0x0f,0xa4,0xc0,17]),0x37fc7)[1])
        self.assertFalse(shift_contract(make([0x66,0x0f,0xa4,0xc0,17]),0x37fc7)[1])
        fault=make([0x0f,0xa4,0xc0,17]);fault['exception']={'number':13}
        self.assertFalse(shift_contract(fault,0x37fc7)[1])

    def test_prefetched_reentry_uses_supplied_instruction_bytes(self):
        initial={name:0 for name in REGS};initial.update(cr0=0x10,eip=0x100,esp=0x200,eflags=2)
        # RET first returns to itself, then consumes the next return pointer.
        test={'idx':0,'hash':'0'*40,'bytes':[0xc3,0xf4],
              'initial':{'regs':initial,'ram':[[0x100,0xc3],[0x200,0],[0x201,1],[0x202,0],[0x203,3],[0x300,0xf4]]},
              'final':{'regs':{'esp':0x204,'eip':0x301},'ram':[]}}
        with tempfile.TemporaryDirectory() as folder:
            path=Path(folder)/'vectors.bin';write_vectors(path,[test])
            p=subprocess.run([str(ROOT/'build/x86-external-runner'),'vectors',str(path),'both'],capture_output=True,text=True)
            self.assertEqual(p.returncode,0,p.stdout+p.stderr)

    def test_runner_rejects_wrong_register_and_memory_results(self):
        runner=ROOT/'build/x86-external-runner'
        self.assertTrue(runner.exists(),'build x86-external-runner before running the oracle checks')
        initial={name:0 for name in REGS};initial.update(cr0=0x10,eip=0x100,eflags=2)
        # ADD AL,1; terminating HLT: AL=1 and IP=103h. No emulator helper
        # computes these expected results for the adapter.
        test={'idx':0,'hash':'0'*40,'bytes':[4,1,0xf4],
              'initial':{'regs':initial,'ram':[[0x100,4],[0x101,1],[0x102,0xf4]]},
              'final':{'regs':{'eax':1,'eip':0x103},'ram':[]}}
        with tempfile.TemporaryDirectory() as folder:
            path=Path(folder)/'vectors.bin';write_vectors(path,[test])
            p=subprocess.run([str(runner),'vectors',str(path),'both'],capture_output=True,text=True)
            self.assertEqual(p.returncode,0,p.stdout+p.stderr)
            test['final']['regs']['eax']=2;write_vectors(path,[test])
            p=subprocess.run([str(runner),'vectors',str(path),'both'],capture_output=True,text=True)
            self.assertEqual(p.returncode,1);self.assertIn('failures=2',p.stdout)
            test['final']['regs']['eax']=1;test['final']['regs']['eflags']=3;write_vectors(path,[test])
            p=subprocess.run([str(runner),'vectors',str(path),'both'],capture_output=True,text=True)
            self.assertEqual(p.returncode,1);self.assertIn('eflags=',p.stdout)
            test['final']['regs']['eflags']=2;test['exception']={'number':13};write_vectors(path,[test])
            p=subprocess.run([str(runner),'vectors',str(path),'both'],capture_output=True,text=True)
            self.assertEqual(p.returncode,1);self.assertIn('exception=-1 expected=13',p.stdout)
            del test['exception']
            test['final']['regs']['eax']=1;test['final']['ram']=[[0x101,2]];write_vectors(path,[test])
            p=subprocess.run([str(runner),'vectors',str(path),'both'],capture_output=True,text=True)
            self.assertEqual(p.returncode,1);self.assertIn('ram[101]',p.stdout)


if __name__=='__main__':unittest.main()
