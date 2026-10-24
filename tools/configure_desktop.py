#!/usr/bin/env python3
"""Switch the working installation's shell from Setup to Program Manager.
Requires the normal installer's first-stage files. Preserves the original INI.
"""
from pathlib import Path
import struct
from prepare_media import FAT

def main():
    fat = FAT(Path('media/dos.raw'))
    win_cluster = next(c for _, n, c, _ in fat.entries() if n == 'WINDOWS    ')
    directory = fat.read_file(win_cluster, fat.spc * 512)
    for off in range(0, len(directory), 32):
        if directory[off:off+11] != b'SYSTEM  INI':
            continue
        cluster, = struct.unpack_from('<H', directory, off+26)
        size, = struct.unpack_from('<I', directory, off+28)
        data = fat.read_file(cluster, size)
        backup = Path('media/system.ini.setup')
        if not backup.exists():
            backup.write_bytes(data)
        data = data.replace(b'shell=setup.exe', b'shell=progman.exe').replace(b'SetupState=1', b'SetupState=0')
        clusters = list(fat.chain(cluster))
        if len(data) > len(clusters) * fat.spc * 512:
            raise ValueError('INI exceeds allocation')
        for i, c in enumerate(clusters):
            fat.f.seek(fat.database + (c-2) * fat.spc * 512)
            fat.f.write(data[i*fat.spc*512:(i+1)*fat.spc*512].ljust(fat.spc*512, b'\0'))
        fat.f.seek(fat.database + (win_cluster-2) * fat.spc * 512 + off + 28)
        fat.f.write(struct.pack('<I', len(data)))
        for _, name, config_cluster, config_size in list(fat.entries()):
            if name == 'CONFIG  SYS':
                config = fat.read_file(config_cluster, config_size)
                config_backup = Path('media/config.sys.before-desktop')
                if not config_backup.exists():
                    config_backup.write_bytes(config)
                if b'HIMEM.SYS' not in config.upper():
                    fat.replace_existing('CONFIG  SYS', b'DEVICE=C:\\DOS\\HIMEM.SYS /TESTMEM:OFF\r\n' + config)
        fat.f.close()
        print('Configured shell=progman.exe; original INI in media/system.ini.setup')
        return
    raise ValueError('Windows SYSTEM.INI not installed')

if __name__ == '__main__':
    main()
