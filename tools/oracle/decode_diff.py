"""Compares hydra-linux's DSP decoder with the reference decoder (run in Unicorn)."""
import os
import random, struct, subprocess, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from oracle import Oracle

DECODE = 0x727430
A = set(range(3, 36)) | {37, 40} | set(range(42, 69)) | set(range(70, 81))
B = {7, 8, 11, 12} | set(range(23, 31)) | {42, 43} | set(range(51, 69)) | {75, 76, 77, 78}
C = {31, 32, 42, 43} | set(range(51, 61)) | {70, 71, 72, 80}
SHIFT = {42, 43, 44, 69, 70, 71, 72}
OPERAND = {2} | set(range(3, 23)) | set(range(33, 39)) | set(range(44, 51)) | {73, 74}
OFFSET = set(range(23, 33))

def fields(op, a, b, c, shift, operand, offset):
    return (op, a if op in A else None, b if op in B else None, c if op in C else None,
            shift if op in SHIFT else None, operand if op in OPERAND else None,
            offset if op in OFFSET else None)

random.seed(7)
fixed = [0x0000, 0x0001, 0x0003, 0x0005, 0x0020, 0x0200, 0x0FFF, 0x1000, 0x2000, 0x3FFF, 0x4000,
         0x4123, 0x5000, 0x5123, 0x6000, 0x7000, 0x7FFF, 0x8000, 0x8001, 0xC000, 0xE000, 0xFFFF]
insns = []
for hi in range(0x10000):
    for lo in fixed + [random.getrandbits(16), random.getrandbits(16) & 0x3FF]:
        insns.append(hi << 16 | lo)

out = subprocess.run([sys.argv[1], 'decode'], input=struct.pack(f'<{len(insns)}I', *insns),
                     capture_output=True, check=True).stdout
o = Oracle(os.environ['HYDRA_REFERENCE'])
mismatches = {}
for i, insn in enumerate(insns):
    op, a, b, c, shift = out[16*i:16*i+5]
    operand, offset = struct.unpack_from('<Ii', out, 16*i + 8)
    mine = fields(op, a, b, c, shift, operand, offset)
    rax, rdx = o.call(DECODE, insn)
    ref = fields(rdx >> 16 & 0xFF, rdx >> 24 & 0xFF, rdx >> 32 & 0xFF, rdx >> 40 & 0xFF, rdx >> 48 & 0xFF,
                 rax >> 32, struct.unpack('<h', struct.pack('<H', rdx & 0xFFFF))[0])
    if mine != ref:
        key = (ref[0], mine[0])
        if key not in mismatches:
            mismatches[key] = (insn, ref, mine, 0)
        mismatches[key] = mismatches[key][:3] + (mismatches[key][3] + 1,)
print(f'{len(insns)} instructions compared, {sum(m[3] for m in mismatches.values())} mismatches')
for (ref_op, my_op), (insn, ref, mine, count) in sorted(mismatches.items()):
    print(f'  ref op {ref_op:2d} / mine {my_op:2d}: x{count}  e.g. {insn:08x}  ref={ref}  mine={mine}')
