"""Read architectural state from SingleStepTests MOO v1 386 files.

Format: https://github.com/dbalsom/moo (Daniel Balsom, MIT).
Cycle chunks are deliberately not decoded: this adapter checks architectural
state, not the 386EX's 16-bit bus timing. Unknown chunks are skipped by length.
"""
import struct
import hashlib

REGS=['cr0','cr3','eax','ebx','ecx','edx','esi','edi','ebp','esp','cs','ds','es','fs','gs','ss','eip','eflags','dr6','dr7']


def chunks(data):
    """Yield bounded, length-delimited chunks; reject truncation and overruns."""
    at=0
    while at<len(data):
        if len(data)-at<8:raise ValueError('Truncated MOO chunk header')
        tag,size=struct.unpack_from('<4sI',data,at);at+=8
        if size>len(data)-at:raise ValueError('MOO chunk exceeds its container')
        yield tag,memoryview(data)[at:at+size]
        at+=size


def state(data):
    """Decode 386 register masks and sparse physical memory bytes."""
    result={'regs':{},'ram':[]}
    for tag,payload in chunks(data):
        if tag in (b'RG32',b'REGS'):
            if len(payload)<4:raise ValueError('Truncated register mask')
            mask=struct.unpack_from('<I',payload)[0]
            if mask>>len(REGS) or len(payload)!=4+mask.bit_count()*4:raise ValueError('Invalid register payload')
            values=iter(struct.unpack_from('<'+'I'*mask.bit_count(),payload,4))
            result['regs']={name:next(values) for i,name in enumerate(REGS) if mask&(1<<i)}
        elif tag==b'RAM ':
            if len(payload)<4:raise ValueError('Truncated RAM count')
            count=struct.unpack_from('<I',payload)[0]
            if len(payload)!=4+count*5:raise ValueError('Invalid RAM payload')
            result['ram']=[list(struct.unpack_from('<IB',payload,4+i*5)) for i in range(count)]
    return result


def read_moo(data):
    """Decode tests, retaining hardware identifiers and exception metadata."""
    top=list(chunks(data))
    if not top or top[0][0]!=b'MOO ':raise ValueError('Missing MOO header')
    header=top[0][1]
    if len(header)<12 or header[0]!=1 or bytes(header[8:12])!=b'386E':raise ValueError('Unsupported MOO version/CPU')
    expected=struct.unpack_from('<I',header,4)[0];tests=[]
    for tag,payload in top[1:]:
        if tag!=b'TEST':continue
        if len(payload)<4:raise ValueError('Truncated test index')
        test={'idx':struct.unpack_from('<I',payload)[0]}
        for tag,p in chunks(payload[4:]):
            if tag in (b'NAME',b'BYTS'):
                if len(p)<4:raise ValueError('Missing counted payload')
                n=struct.unpack_from('<I',p)[0]
                if n!=len(p)-4:raise ValueError('Invalid counted payload')
                test['name' if tag==b'NAME' else 'bytes']=bytes(p[4:]).decode('utf-8') if tag==b'NAME' else list(p[4:])
            elif tag in (b'INIT',b'FINA'):test['initial' if tag==b'INIT' else 'final']=state(p)
            elif tag==b'HASH':
                if len(p)!=20:raise ValueError('Invalid hardware vector hash')
                test['hash']=bytes(p).hex()
            elif tag==b'EXCP':
                if len(p)!=5:raise ValueError('Invalid exception payload')
                number,address=struct.unpack('<BI',p);test['exception']={'number':number,'flag_address':address}
        if not all(k in test for k in ['initial','final','hash','bytes']):raise ValueError('Incomplete architectural vector')
        tests.append(test)
    if len(tests)!=expected:raise ValueError('MOO test count mismatch')
    return tests


def write_vectors(path,tests):
    """Serialize a strict endian-stable input for the native CPU runner."""
    out=bytearray(b'PCX86V2\0'+struct.pack('<I',len(tests)))
    for test in tests:
        name=(test.get('opcode','')+'#'+str(test['idx'])+' '+test['hash']+' '+test.get('name','')).encode()
        out+=struct.pack('<I',len(name))+name
        ex=test.get('exception',{})
        out+=struct.pack('<IIII',test.get('flagMask',0x37fd7),ex.get('number',0xffffffff),ex.get('flag_address',0xffffffff),len(test['bytes'])-1)
        initial=test['initial']['regs'];expected=dict(initial,**test['final']['regs'])
        if any(name not in initial for name in REGS):raise ValueError('Initial register state incomplete')
        for regs in [initial,expected]:out+=struct.pack('<20I',*[regs[name] for name in REGS])
        before=dict(test['initial']['ram']);after=dict(before);after.update(test['final']['ram'])
        for ram in [before,after]:
            out+=struct.pack('<I',len(ram))
            for address,value in sorted(ram.items()):
                if not 0<=address<16*1024*1024 or not 0<=value<256:raise ValueError('Invalid RAM byte')
                out+=struct.pack('<IB',address,value)
    path.write_bytes(out)
