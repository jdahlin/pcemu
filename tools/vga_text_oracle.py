"""Independent IBM VGA text scan-line oracle for captured external diagnostics.

Reads read-only register/plane dumps, not PCEmu rendering helpers. Captures omit
cursor animation and use the visible blink phase. IBM 42G2193, PDF 87/101/115/118.
https://bitsavers.trailing-edge.com/pdf/ibm/pc/ps2/42G2193_PS2_Hardware_Interface_Technical_Reference_Video_Subsystems_Sep92.pdf
"""
from PIL import Image


def render_text(state):
    """Resolve physical scan lines, character/font addresses and DAC colors."""
    c,s,a=state['crtc'],state['seq'],state['attr']
    memory=[bytes.fromhex(value) for value in state['planes']]
    cell_width=8 if s[1]&1 else 9
    clock_repeat=2 if s[1]&8 else 1
    glyph_height=(c[9]&31)+1
    width=(c[1]+1)*cell_width*clock_repeat
    height=(c[0x12]|((c[7]&2)<<7)|((c[7]&64)<<3))+1
    split=c[0x18]+((c[7]>>4)&1)*256+((c[9]>>6)&1)*512
    map_b=(s[3]&3)+((s[3]>>4)&1)*4
    map_a=((s[3]>>2)&3)+((s[3]>>5)&1)*4
    colors=[]
    for color in range(16):
        index=a[color&a[0x12]]&63
        if a[0x10]&128:index=(index&15)|((a[0x14]&3)<<4)
        index|=(a[0x14]&12)<<4
        colors.append(tuple(state['dac'][index&state['dacMask']]))
    result=Image.new('RGB',(width,height));output=[]
    for y in range(height):
        bottom=y>split
        scan=(y-split-1 if bottom else y)//(2 if c[9]&128 else 1)
        if not bottom:scan+=c[8]&31
        row,glyph_line=divmod(scan,glyph_height)
        address=(0 if bottom else c[0x0c]*256+c[0x0d])+row*c[0x13]*2
        if not bottom:address+=(c[8]>>5)&3
        pan=a[0x13]&15
        if cell_width==9:pan=pan+1 if pan<8 else 0
        else:pan&=7
        if bottom and a[0x10]&32:pan=0
        for x in range(width):
            col,dot=divmod(x//clock_repeat+pan,cell_width)
            at=(address+col)&65535
            char,attr=memory[0][at],memory[1][at]
            font=map_a if attr&8 else map_b
            font_base=(font%4)*16384+(font//4)*8192
            bits=memory[2][font_base+char*32+glyph_line]
            on=bool(bits&(128>>dot)) if dot<8 else bool(a[0x10]&4 and 0xc0<=char<=0xdf and bits&1)
            if attr&0x77==1 and glyph_line==c[0x14]&31:on=True
            fg=attr&(7 if map_a!=map_b else 15)
            bg=(attr>>4)&(7 if a[0x10]&8 else 15)
            output.append(colors[fg if on else bg])
    result.putdata(output)
    return result
