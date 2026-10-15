"""Builds the wave ROM with the reference's hydra::buildWaveRom (in Unicorn)
and compares it with ours.  usage: waverom_diff.py <firmware.dat> <our-rom.bin>"""
import math, os, struct, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import oracle
oracle.HEAP_SIZE = 0x20000000
from oracle import Oracle
from unicorn import UC_HOOK_CODE
from unicorn.x86_const import *

BUILD, FIND = 0x725980, 0x6D4740
NEW, DELETE, BZERO, PATTERN16, SINCOS, EXP10 = 0xAA8B2A, 0xAA8B1E, 0xAA8B42, 0xAA8EBA, 0xAA8BAE, 0xAA8B90

def sections(path):
    d = open(path, 'rb').read(); off = 0x5B; out = {}
    while off + 44 <= len(d):
        name = d[off+20:off+40].split(b'\0')[0].decode(); size, = struct.unpack_from('<I', d, off + 40)
        out[name] = d[off+44:off+44+size]; off += 44 + size
    return out

code = sections(sys.argv[1])['A5 code']
o = Oracle(os.environ['HYDRA_REFERENCE'])
mu = o.mu
SECTION_DATA = o.alloc(len(code), code)
SECTION = o.alloc(72)  # fake FirmwareSection: data vector at +48
mu.mem_write(SECTION + 48, struct.pack('<QQQ', SECTION_DATA, SECTION_DATA + len(code), SECTION_DATA + len(code)))
RESULT = o.alloc(24)
OPTIONS = o.alloc(24)  # empty vector<double>: no per-wave gains
for stub in (NEW, DELETE, BZERO, PATTERN16, SINCOS, EXP10, FIND):
    mu.mem_write(stub, b'\xc3')

def hook(uc, address, size, user):
    rdi, rsi, rdx = (uc.reg_read(r) for r in (UC_X86_REG_RDI, UC_X86_REG_RSI, UC_X86_REG_RDX))
    if address == NEW:
        uc.reg_write(UC_X86_REG_RAX, o.alloc(rdi))
    elif address == BZERO:
        uc.mem_write(rdi, bytes(rsi))
    elif address == PATTERN16:
        pattern = bytes(uc.mem_read(rsi, 16))
        uc.mem_write(rdi, (pattern * (rdx // 16 + 1))[:rdx])
    elif address == SINCOS:
        x = struct.unpack('<d', struct.pack('<Q', uc.reg_read(UC_X86_REG_XMM0) & (2**64 - 1)))[0]
        uc.reg_write(UC_X86_REG_XMM0, struct.unpack('<Q', struct.pack('<d', math.sin(x)))[0])
        uc.reg_write(UC_X86_REG_XMM1, struct.unpack('<Q', struct.pack('<d', math.cos(x)))[0])
    elif address == FIND:
        uc.reg_write(UC_X86_REG_RAX, SECTION)
    elif address == EXP10:
        raise RuntimeError('unexpected exp10')

for stub in (NEW, DELETE, BZERO, PATTERN16, SINCOS, EXP10, FIND):
    mu.hook_add(UC_HOOK_CODE, hook, begin=stub, end=stub)
o.call(BUILD, RESULT, 0, OPTIONS, limit=0)
begin, end = struct.unpack('<QQ', mu.mem_read(RESULT, 16))
ref = bytes(mu.mem_read(begin, end - begin))
mine = open(sys.argv[2], 'rb').read()
print(f'reference {len(ref)} bytes, ours {len(mine)} bytes')
if len(ref) != len(mine):
    sys.exit(1)
header_diff = sum(1 for a, b in zip(ref[:0x10000], mine[:0x10000]) if a != b)
r = struct.unpack(f'<{(len(ref) - 0x10000) // 2}h', ref[0x10000:])
m = struct.unpack(f'<{(len(mine) - 0x10000) // 2}h', mine[0x10000:])
diffs = [abs(a - b) for a, b in zip(r, m) if a != b]
print(f'header bytes differing: {header_diff}')
print(f'samples: {len(r)}, differing: {len(diffs)}, max abs difference: {max(diffs) if diffs else 0}')
if header_diff:
    runs, start = [], None
    for i in range(0x10000):
        d = ref[i] != mine[i]
        if d and start is None: start = i
        if not d and start is not None: runs.append((start, i)); start = None
    for a, b in runs[:12]:
        print(f'  header {a:#06x}-{b:#06x}: ref {ref[a & ~3:(b + 3) & ~3][:32].hex()}  ours {mine[a & ~3:(b + 3) & ~3][:32].hex()}')
