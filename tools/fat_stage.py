#!/usr/bin/env python3
"""Small FAT16 writer for private emulator test disks (no mounting or external emulator)."""
import struct
from pathlib import Path
from prepare_media import FAT

class Writer(FAT):
    def __init__(self, path):
        super().__init__(path)
        if self.bits != 16:
            raise ValueError('FAT16 required')
        self.cluster_bytes = self.spc * 512
        self.next_free = 2
        self.f.seek(self.base)
        boot = self.f.read(512)
        total = struct.unpack_from('<H', boot, 19)[0] or struct.unpack_from('<I', boot, 32)[0]
        self.cluster_limit = 2 + (total - (self.database - self.base)//512)//self.spc

    def allocate(self, data):
        need = (len(data)+self.cluster_bytes-1)//self.cluster_bytes
        free = []
        while len(free) < need:
            if self.next_free >= min(self.cluster_limit, 0xfff0):
                raise ValueError('Disk full')
            c = self.next_free
            self.next_free += 1
            if struct.unpack_from('<H', self.fat, c*2)[0] == 0:
                free.append(c)
        for i, c in enumerate(free):
            struct.pack_into('<H', self.fat, c*2, free[i+1] if i+1<len(free) else 0xffff)
            self.f.seek(self.database+(c-2)*self.cluster_bytes)
            self.f.write(data[i*self.cluster_bytes:(i+1)*self.cluster_bytes].ljust(self.cluster_bytes,b'\0'))
        return free[0] if free else 0

    @staticmethod
    def entry(name, attr, cluster, size):
        if len(name) != 11 or ('.' in name and not name.startswith('.')):
            parts=name.upper().split('.')
            if len(parts)>2 or len(parts[0])>8 or (len(parts)==2 and len(parts[1])>3):
                raise ValueError('Not an 8.3 name: '+name)
            name=parts[0].ljust(8)+(parts[1] if len(parts)==2 else '').ljust(3)
        e=bytearray(32);e[:11]=name.encode('ascii');e[11]=attr
        struct.pack_into('<H',e,24,(16<<9)|33)
        struct.pack_into('<H',e,26,cluster);struct.pack_into('<I',e,28,size)
        return e

    def root_entry(self, entry):
        self.f.seek(self.rootbase)
        root=self.f.read(self.nroot*32)
        for off in range(0,len(root),32):
            if root[off:off+11] == entry[:11]:
                raise ValueError('Entry already exists: '+str(entry[:11]))
        off=next(i for i in range(0,len(root),32) if root[i] in (0,229))
        self.f.seek(self.rootbase+off);self.f.write(entry)

    def put_directory(self, name, files):
        size=((len(files)+3)*32+self.cluster_bytes-1)//self.cluster_bytes*self.cluster_bytes
        start=self.allocate(bytes(size))
        directory=self.entry('.          ',16,start,0)+self.entry('..         ',16,0,0)
        for filename, data in files.items():
            directory+=self.entry(filename,32,self.allocate(data),len(data))
        directory=directory.ljust(size,b'\0')
        for i,c in enumerate(self.chain(start)):
            self.f.seek(self.database+(c-2)*self.cluster_bytes)
            self.f.write(directory[i*self.cluster_bytes:(i+1)*self.cluster_bytes])
        self.root_entry(self.entry(name,16,start,0))
        self.commit()

    def commit(self):
        for i in range(self.nfats):
            self.f.seek(self.fatbase+i*self.spf*512);self.f.write(self.fat)
        self.f.flush()
