"""Runs random DSP programs on hydra-linux's DspCore and on the reference
DspCore::runImpl<false> (in Unicorn) and compares the resulting state."""
import os
import random, struct, subprocess, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from oracle import Oracle, HEAP
from unicorn import UC_HOOK_CODE
from unicorn.x86_const import *

RUN_IMPL, DECODE_STUB_MEMCMP = 0x72C730, 0xAA8EA2
SHARED_WORDS, MASK = 0x17C00, 0xFFFFFFFF
def initial(space, a): return (a * 2654435761 + space * 0x9E3779B9) & MASK

dsptest = sys.argv[1]
cases = int(sys.argv[2]) if len(sys.argv) > 2 else 300
budget = int(sys.argv[3]) if len(sys.argv) > 3 else 300
seed = int(sys.argv[4]) if len(sys.argv) > 4 else 1
rng = random.Random(seed)

# Valid instruction words, found with our own decoder (identical to the reference's).
pool_raw = [rng.getrandbits(32) for _ in range(400000)]
dec = subprocess.run([dsptest, 'decode'], input=struct.pack(f'<{len(pool_raw)}I', *pool_raw),
                     capture_output=True, check=True).stdout
by_op = {}
for i, insn in enumerate(pool_raw):
    op = dec[16 * i]
    if op != 1:
        by_op.setdefault(op, []).append(insn)
# Add structured encodings for rare ops.
extra = []
for a in range(16):
    for b in range(16):
        for c in range(16):
            sh = rng.randrange(32)
            extra += [0x80000000 | a << 24 | b << 20 | c | sh << 5, 0x80040000 | a << 24 | b << 20 | c | sh << 5,
                      0xC0000000 | a << 24 | b << 20 | rng.choice([0, 1, 2, 3, 4, 5, 8, 0xA, 0xD, 0xC]) << 16 | c,
                      0xE0000000 | a << 24 | b << 20 | rng.randrange(2, 10) << 16,
                      0xF0040000 | a << 24 | b << 20 | rng.choice([0, 0x5000, 0x6000, 0x7000]),
                      0xF0060000 | a << 24 | c, 0xF0020000 | a << 24 | a,
                      0xD0000000 | a << 24 | rng.choice([0, 2, 6]) << 16 | c | sh << 5,
                      0x90000000 | a << 24 | b << 20 | rng.choice([0, 2, 4, 6]) << 16 | rng.choice([0, 0x4000, 0x5000]) | rng.getrandbits(9),
                      0x90080000 | a << 24 | rng.choice([0, 2]) << 16 | rng.randrange(32) << 9 | rng.getrandbits(9),
                      0xE0000000 | rng.randrange(1, 32) << 5,
                      0xB00F0000 | a << 24 | rng.getrandbits(16)]
dec = subprocess.run([dsptest, 'decode'], input=struct.pack(f'<{len(extra)}I', *extra),
                     capture_output=True, check=True).stdout
for i, insn in enumerate(extra):
    op = dec[16 * i]
    if op != 1:
        by_op.setdefault(op, []).append(insn)
ops = sorted(by_op)
print(f'{len(ops)} distinct ops available for program generation')

def reg_value():
    k = rng.random()
    if k < 0.3: return rng.randrange(0, 600)
    if k < 0.45: return rng.randrange(0x8000, 0x20000)
    if k < 0.55: return rng.choice([0, 1, 2, 0x7FFFFFFF, 0x80000000, 0xFFFFFFFF, 0x10000, 0xFFFF])
    return rng.getrandbits(32)

def make_case():
    program = [rng.choice(by_op[rng.choice(ops)]) for _ in range(4096)]
    regs = [reg_value() for _ in range(16)]
    regs[15] = rng.choice([0, 1, 3, rng.getrandbits(32)])
    pc = rng.randrange(4096)
    pending = rng.choice([0, 0, 2, 4, 6])
    data = [reg_value() for _ in range(512)]
    return budget, regs, pc, pending, program, data

# --- reference -------------------------------------------------------------
o = Oracle(os.environ['HYDRA_REFERENCE'])
mu = o.mu
CORE = o.alloc(0x10200)
PROGRAM = o.alloc(4096 * 4)
DATA = o.alloc(512 * 4)
SHARED = o.alloc(SHARED_WORDS * 4)
STUBS = o.alloc(0x100, b'\xc3' * 0x100)       # one byte per host method (ret, but hooked)
VTABLE = o.alloc(0x100)
HOSTOBJ = o.alloc(16)
mu.mem_write(HOSTOBJ, struct.pack('<Q', VTABLE))
shared_init = b''.join(struct.pack('<I', initial(3, i)) for i in range(SHARED_WORDS))
host_writes = {}

def host_get(space, a): return host_writes.get((space, a), initial(space, a))

def on_stub(uc, address, size, user):
    slot = address - STUBS
    core, a, v = uc.reg_read(UC_X86_REG_RSI), uc.reg_read(UC_X86_REG_RDX) & MASK, uc.reg_read(UC_X86_REG_RCX) & MASK
    method = {16: ('r', 0), 24: ('w', 0), 32: ('r', 1), 40: ('w', 1), 64: ('r', 2), 72: ('w', 2)}.get(slot)
    if method is None:
        raise RuntimeError(f'unexpected host method slot {slot}')
    kind, space = method
    if kind == 'r':
        uc.reg_write(UC_X86_REG_RAX, host_get(space, a))
    else:
        host_writes[(space, a)] = v

def on_memcmp(uc, address, size, user):
    s1, s2, n = uc.reg_read(UC_X86_REG_RDI), uc.reg_read(UC_X86_REG_RSI), uc.reg_read(UC_X86_REG_RDX)
    a, b = bytes(uc.mem_read(s1, n)), bytes(uc.mem_read(s2, n))
    uc.reg_write(UC_X86_REG_RAX, 0 if a == b else (1 if a > b else 0xFFFFFFFF))  # the patched 'ret' returns

# Host methods: vtable offset X points to STUBS + X (a 'ret'); the hook supplies the behaviour.
mu.mem_write(VTABLE, b''.join(struct.pack('<Q', STUBS + 8 * i) for i in range(32)))
mu.hook_add(UC_HOOK_CODE, on_stub, begin=STUBS, end=STUBS + 0xFF)
mu.mem_write(DECODE_STUB_MEMCMP, b'\xc3')
mu.hook_add(UC_HOOK_CODE, on_memcmp, begin=DECODE_STUB_MEMCMP, end=DECODE_STUB_MEMCMP)

import collections
TRACE = collections.deque(maxlen=64)
if '--trace' in sys.argv:
    mu.hook_add(UC_HOOK_CODE, lambda uc, a, sz, u: TRACE.append(a))

def reference(case):
    budget_, regs, pc, pending, program, data = case
    host_writes.clear()
    mu.mem_write(CORE, bytes(0x10200))
    mu.mem_write(PROGRAM, struct.pack('<4096I', *program))
    mu.mem_write(DATA, struct.pack('<512I', *data))
    mu.mem_write(SHARED, shared_init)
    w = lambda off, fmt, v: mu.mem_write(CORE + off, struct.pack(fmt, v))
    w(176, '<Q', SHARED); w(184, '<i', 0); w(192, '<Q', HOSTOBJ); w(200, '<Q', PROGRAM); w(208, '<Q', DATA)
    w(216, '<I', 0x1FF); w(220, '<I', 0xFFFFFFFF); w(224, '<B', 0)
    mu.mem_write(CORE + 228, struct.pack('<16I', *regs))
    w(65916, '<I', pc); w(65920, '<I', pending); w(65912, '<i', -1); w(0x10130, '<q', -1)
    try:
        executed, _ = o.call(RUN_IMPL, CORE, budget_)
    except Exception as e:
        import collections
        print('crash; last instructions:')
        for ea in list(TRACE)[-25:]:
            print(f'  {ea:x}')
        raise RuntimeError(f'{e} at rip={mu.reg_read(UC_X86_REG_RIP):x}')
    r = lambda off, fmt: struct.unpack(fmt, mu.mem_read(CORE + off, struct.calcsize(fmt)))[0]
    regs_out = list(struct.unpack('<16I', mu.mem_read(CORE + 228, 64)))
    status = [r(65916, '<I'), r(65920, '<I'), r(65992, '<B'), r(65993, '<B')]
    data_out = list(struct.unpack('<512I', mu.mem_read(DATA, 2048)))
    sh = bytes(mu.mem_read(SHARED, SHARED_WORDS * 4))
    changed = {i: struct.unpack_from('<I', sh, 4 * i)[0] for i in range(SHARED_WORDS) if sh[4*i:4*i+4] != shared_init[4*i:4*i+4]}
    return executed & MASK, regs_out, status, data_out, changed, dict(host_writes)

# --- ours --------------------------------------------------------------------
def ours(all_cases):
    blob = b''.join(struct.pack('<I', c[0]) + struct.pack('<16I', *c[1]) + struct.pack('<II', c[2], c[3]) +
                    struct.pack('<4096I', *c[4]) + struct.pack('<512I', *c[5]) for c in all_cases)
    out = subprocess.run([dsptest, 'run'], input=blob, capture_output=True, check=True).stdout
    pos, results = 0, []
    def take(n):
        nonlocal pos
        v = struct.unpack_from(f'<{n}I', out, pos); pos += 4 * n; return list(v)
    for _ in all_cases:
        executed = take(1)[0]; regs = take(16); status = take(4); data = take(512)
        n = take(1)[0]; pairs = take(2 * n); changed = dict(zip(pairs[0::2], pairs[1::2]))
        n = take(1)[0]; triples = take(3 * n)
        writes = {(triples[i], triples[i+1]): triples[i+2] for i in range(0, len(triples), 3)}
        results.append((executed, regs, status, data, changed, writes))
    return results

all_cases = [make_case() for _ in range(cases)]
mine = ours(all_cases)
failures = 0
for i, case in enumerate(all_cases):
    ref = reference(case)
    names = ('executed', 'regs', 'status(pc,pending,inIrq,trapped)', 'data', 'shared', 'host writes')
    diffs = [n for n, x, y in zip(names, ref, mine[i]) if x != y]
    if diffs:
        failures += 1
        if failures <= 5:
            print(f'case {i}: differs in {diffs}')
            for n, x, y in zip(names, ref, mine[i]):
                if x != y and n in ('executed', 'regs', 'status(pc,pending,inIrq,trapped)'):
                    print(f'    {n}: ref={x}\n    {"":{len(n)}}  mine={y}')
print(f'{cases} cases x {budget} instructions: {failures} failures')

if '--bisect' in sys.argv:
    import collections
    seen = collections.Counter()
    examples = {}
    for i, case in enumerate(all_cases):
        if reference(case)[:6] == ours([case])[0][:6]:
            continue
        for k in range(1, case[0] + 1):
            c = (k,) + case[1:]
            r, m = reference(c), ours([c])[0]
            if r != m:
                prev = reference((k - 1,) + case[1:]) if k > 1 else None
                pc = prev[2][0] if prev else case[2]
                insn = case[4][pc]
                dec = subprocess.run([dsptest, 'decode'], input=struct.pack('<I', insn), capture_output=True).stdout
                op = dec[0]
                seen[op] += 1
                if op not in examples:
                    names = ('executed', 'regs', 'status', 'data', 'shared', 'writes')
                    diff = [(n, x, y) for n, x, y in zip(names, r, m) if x != y]
                    before = prev[1] if prev else case[1]
                    examples[op] = (insn, pc, before, diff)
                break
    for op, count in seen.most_common():
        insn, pc, before, diff = examples[op]
        print(f'op {op}: {count} cases; e.g. insn {insn:08x} at pc {pc:03x}')
        print(f'   regs before: {[hex(x) for x in before]}')
        for n, x, y in diff:
            if n in ('regs', 'status', 'executed'):
                print(f'   {n}: ref={[hex(v) for v in x] if isinstance(x, list) else x}')
                print(f'   {"":{len(n)}}  mine={[hex(v) for v in y] if isinstance(y, list) else y}')
            else:
                print(f'   {n} differ')
