"""Disassembles DSP program words using dsptest's decoder.
usage: dspdis.py <dsptest> <program.bin> [core] [start] [count]"""
import struct, subprocess, sys

NAMES = ['nop', 'invalid', 'addeight', 'ioload', 'iostore', 'extload', 'extstore', 'ioloadx', 'iostorex',
         'load', 'store', 'loadx', 'storex', 'addi', 'subi', 'movi', 'addil', 'subil', 'movil', 'andil', 'oril',
         'movih', 'andih', 'bgt', 'bge', 'beq', 'bne', 'bhieqlo', 'bhinelo', 'bloeq', 'blone', 'bbs', 'bbc',
         'jz', 'jnz', 'loop', 'call', 'jal', 'jmp', 'ret', 'jr', 'reti', 'mul', 'mac', 'mulm', 'addm', 'addmsats',
         'addmsat', 'subm', 'dlyrd', 'dlywr', 'add', 'addsat', 'sub', 'subsat', 'and', 'or', 'shl', 'sar', 'max',
         'min', 'ldr', 'ldr+', 'str', 'str+', 'iold', 'iold+', 'iost', 'iost+', 'biquad', 'sari', 'shli', 'shri',
         'portrd', 'portwr', 'mov', 'movhl', 'movlh', 'movh', 'negsat', 'abssat']

dsptest, path = sys.argv[1], sys.argv[2]
core = int(sys.argv[3], 0) if len(sys.argv) > 3 else 0
start = int(sys.argv[4], 0) if len(sys.argv) > 4 else 0
count = int(sys.argv[5], 0) if len(sys.argv) > 5 else 64
data = open(path, 'rb').read()[core * 0x4000:(core + 1) * 0x4000]
words = struct.unpack('<4096I', data)[start:start + count]
out = subprocess.run([dsptest, 'decode'], input=struct.pack(f'<{len(words)}I', *words), capture_output=True).stdout
for i, w in enumerate(words):
    op, a, b, c, shift = out[16 * i:16 * i + 5]
    operand, offset = struct.unpack_from('<Ii', out, 16 * i + 8)
    pc = start + i
    name = NAMES[op]
    if 23 <= op <= 32:
        args = f'r{a}, {"bit " + str(c) if op in (31, 32) else "r" + str(b)} -> {(pc + offset + 1) & 0xFFF:03x}'
    elif op in (33, 34, 35, 36, 37, 38):
        args = f'r{a}, {operand:03x}'
    elif op in (42, 43):
        args = f'r{c} = r{a}*r{b} >> {shift}'
    elif op in (51, 52, 53, 54, 55, 56, 57, 58, 59, 60):
        args = f'r{c} = r{a}, r{b}'
    elif op in (70, 71, 72):
        args = f'r{c} = r{a}, {shift}'
    elif op in (61, 62, 63, 64, 65, 66, 67, 68, 75, 76, 77, 78):
        args = f'r{a}, r{b}'
    elif op == 69:
        args = f'>> {shift}'
    else:
        args = f'r{a}, r{b}, {operand:#x}'
    print(f'{pc:03x}: {w:08x}  {name:8s} {args}')
