"""Original VGA register/parameter tables derived from IBM 42G2193 section 2.

Values describe the documented 320/640-pixel, 200/350/480-line VGA modes;
they are hardware programming data, not firmware executable code.
https://bitsavers.trailing-edge.com/pdf/ibm/pc/ps2/42G2193_PS2_Hardware_Interface_Technical_Reference_Video_Subsystems_Sep92.pdf#page=40
"""


def graphics_tables():
    """Encode each mode's geometry and complete sequencer/CRTC/GFX/AC groups."""
    crtc_200 = [0x5f,0x4f,0x50,0x82,0x54,0x80,0xbf,0x1f,0,0xc0,0,0,0,0,0,0,
                0x9c,0x8e,0x8f,0x28,0,0x96,0xb9,0xe3,0xff]
    crtc_350 = [0x5b,0x4f,0x53,0x37,0x52,0,0x6c,0x1f,0,0x40,0,0,0,0,0,0,
                0x5e,0x8e,0x5d,0x28,0x0f,0x5f,0x0a,0xe3,0xff]
    crtc_480 = [0x5f,0x4f,0x50,0x82,0x54,0x80,0x0b,0x3e,0,0x40,0,0,0,0,0,0,
                0xea,0x8c,0xdf,0x28,0,0xe7,4,0xe3,0xff]
    specs = [(0x0d,320,200,0x2000,4,8,0x63),(0x0e,640,200,0x4000,4,8,0x63),
             (0x0f,640,350,0x8000,1,14,0xa3),(0x10,640,350,0x8000,4,14,0xa3),
             (0x11,640,480,0xa000,1,16,0xe3),(0x12,640,480,0xa000,4,16,0xe3),
             (0x13,320,200,0xfa00,8,8,0x63)]
    records = []
    for mode,w,h,page,bpp,font,misc in specs:
        crtc = list({200:crtc_200,350:crtc_350,480:crtc_480}[h])
        seq = [3,1,15,0,6]
        gfx = [0,0,0,0,0,0,5,15,255]
        attr = [0,1,2,3,4,5,20,7,56,57,58,59,60,61,62,63,1,0,15,0,0]
        if w == 320 and bpp != 8:
            seq[1] = 9
            crtc[:6] = [0x2d,0x27,0x28,0x90,0x2b,0xa0]
            crtc[0x13] = 0x14
        if bpp == 1:
            seq[2] = 1
            attr[:16] = [0,7]*8
        if bpp == 8:
            seq[4] = 14
            gfx[5] = 0x40
            crtc[9] = 0x41
            crtc[0x14] = 0x40
            crtc[0x17] = 0xa3
            attr = list(range(16)) + [0x41,0,15,0,0]
        records.append((mode,w,h,page,bpp,font,misc,seq,crtc,gfx,attr))
    cga_crtc=[0x2d,0x27,0x28,0x90,0x2b,0xa0,0xbf,0x1f,0,0xc0,0,0,0,0,0,0,
              0x9c,0x8e,0x8f,0x14,0,0x96,0xb9,0xa2,0xff]
    for mode in (4,5,6):
        crtc=list(cga_crtc)
        seq=[3,9,3,0,2]
        gfx=[0,0,0,0,0,0x30,0x0f,15,255]
        attr=[0,2,4,20]+[0]*12+[1,0,3,0,0]
        if mode==5:attr[:4]=[0,3,4,7]
        if mode==6:
            crtc[:6]=[0x5f,0x4f,0x50,0x82,0x54,0x80]
            crtc[0x13]=0x28
            crtc[0x17]=0xc2
            seq=[3,1,1,0,6]
            gfx[5],gfx[6]=0,0x0d
            attr=[0,63]+[0]*14+[1,0,1,0,0]
        records.append((mode,640 if mode==6 else 320,200,0x4000,1 if mode==6 else 2,8,0x63,seq,crtc,gfx,attr))
    return sorted(records)


def assembly_tables():
    """Emit 70-byte mode descriptors and standard 64-byte parameter records."""
    records = graphics_tables()
    lines = ['; Generated VGA register tables; source and derivation in tools/vga_modes.py.',
             '%define MODE_RECORD_BYTES 70', '%define MODE_WIDTH 1', '%define MODE_HEIGHT 3',
             '%define MODE_PAGE 5', '%define MODE_BPP 7', '%define MODE_FONT 8',
             '%define MODE_MISC 9', '%define MODE_SEQ 10', '%define MODE_CRTC 15',
             '%define MODE_GFX 40', '%define MODE_ATTR 49', 'graphics_mode_pointers:']
    lines += ['dw ' + (f'mode_{i:02x}' if i in {r[0] for r in records} else '0') for i in range(32)]
    for mode,w,h,page,bpp,font,misc,seq,crtc,gfx,attr in records:
        lines += [f'mode_{mode:02x}:', f'db {mode}', f'dw {w},{h},{page}',
                  f'db {bpp},{font},{misc}', 'db '+','.join(map(str,seq+crtc+gfx+attr))]
    lines += ['video_parameters:']
    for i in range(32):
        record = next((r for r in records if r[0] == i), None)
        if record:
            _,w,h,page,bpp,font,misc,seq,crtc,gfx,attr = record
        else:
            w,h,page,font,misc= (320 if i<2 else 640),400,(2048 if i<2 else 4096),16,0x67
            seq=[3,0 if i>=2 else 8,3,0,2]
            crtc=[0x5f,0x4f,0x50,0x82,0x55,0x81,0xbf,0x1f,0,0x4f,13,14,0,0,0,0,
                  0x9c,0x8e,0x8f,0x28,0x1f,0x96,0xb9,0xa3,0xff]
            gfx=[0,0,0,0,0,0x10,0x0e,0,255]
            attr=[0,1,2,3,4,5,20,7,56,57,58,59,60,61,62,63,0x0c,0,15,8,0]
            if i<2:crtc[:6]=[0x2d,0x27,0x28,0x90,0x2b,0xa0]
            if i==7:font,h,misc,gfx[6]=14,350,0x66,0x0a
        lines += [f'db {w//8},{h//font-1},{font}', f'dw {page}',
                  'db '+','.join(map(str,seq[1:]+[misc]+crtc+attr[:20]+gfx))]
    # Standard VGA 256-color palette: 16 CGA colors, 16 grays, nine 24-hue rings.
    palette=[]
    for i in range(16):
        palette.extend([42*bool(i&4)+21*bool(i&8),42*bool(i&2)+21*bool(i&8),42*bool(i&1)+21*bool(i&8)])
    palette[6*3+1]=21
    for level in [0,5,8,11,14,17,20,24,28,32,36,40,45,50,56,63]:palette.extend([level]*3)
    for high,low in [(63,0),(63,31),(63,45),(28,0),(28,14),(28,20),(16,0),(16,8),(16,11)]:
        levels=[low+((high-low)*i+1)//4 for i in range(5)]
        for edge in range(6):
            for i in range(4):
                t=levels[i]
                palette.extend([(t,low,high),(high,low,high-(t-low)),(high,t,low),
                                (high-(t-low),high,low),(low,high,t),(low,high-(t-low),high)][edge])
    palette.extend([0]*(768-len(palette)))
    lines += ['graphics_dac:']
    lines += ['db '+','.join(map(str,palette[i:i+24])) for i in range(0,768,24)]
    return '\n'.join(lines)+'\n'


def vbe_modes():
    """VESA standard mode identifiers and Bochs-compatible 32-bit OEM modes."""
    result=[(0x100,640,400,8),(0x101,640,480,8),(0x103,800,600,8),
            (0x105,1024,768,8),(0x107,1280,1024,8)]
    for first,w,h in [(0x110,640,480),(0x113,800,600),(0x116,1024,768),(0x119,1280,1024)]:
        result += [(first+i,w,h,bpp) for i,bpp in enumerate((15,16,24))]
    result += [(0x140+i,w,h,32) for i,(w,h) in enumerate(((320,200),(640,400),(640,480),(800,600),(1024,768),(1280,1024)))]
    result += [(0x120,1600,1200,8),(0x121,1600,1200,15),(0x122,1600,1200,16),(0x123,1600,1200,24),(0x147,1600,1200,32)]
    return sorted(result)


def vbe_assembly():
    """Publish only modes supported by PCEmu's 16-MiB, 1920x1200 DISPI board."""
    lines=['; Generated VBE mode list/descriptors; see tools/vga_modes.py.',
           'vbe_mode_list:', 'dw '+','.join(str(m[0]) for m in vbe_modes())+',0xffff',
           'vbe_modes:']
    for mode,w,h,bpp in vbe_modes():
        lines += [f'dw {mode},{w},{h}',f'db {bpp},0']
    lines += ['vbe_modes_end:']
    return '\n'.join(lines)+'\n'
