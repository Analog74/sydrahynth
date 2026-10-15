"""Runs pure functions of the reference plugin (x86_64 Mach-O slice) inside Unicorn."""
import os
import struct
from unicorn import Uc, UC_ARCH_X86, UC_MODE_64, UC_PROT_ALL, UcError
from unicorn.x86_const import *

STACK, STACK_SIZE, SENTINEL = 0x7F0000000000, 0x100000, 0x7E0000000000
HEAP, HEAP_SIZE = 0x600000000000, 0x4000000

class Oracle:
    def __init__(self, path):
        data = open(path, 'rb').read()
        self.mu = Uc(UC_ARCH_X86, UC_MODE_64)
        ncmds, = struct.unpack_from('<I', data, 16)
        off = 32
        for _ in range(ncmds):
            cmd, size = struct.unpack_from('<II', data, off)
            if cmd == 0x19:  # LC_SEGMENT_64
                name = data[off+8:off+24].split(b'\0')[0].decode()
                vmaddr, vmsize, fileoff, filesize = struct.unpack_from('<QQQQ', data, off + 24)
                if vmsize and name != '__PAGEZERO':
                    start, end = vmaddr & ~0xFFF, (vmaddr + vmsize + 0xFFF) & ~0xFFF
                    self.mu.mem_map(start, end - start, UC_PROT_ALL)
                    self.mu.mem_write(vmaddr, data[fileoff:fileoff + filesize])
            off += size
        # Data segments with Mach-O chained fixups already applied (exported from IDA).
        try:
            fixed = open(os.environ.get('HYDRA_FIXED_DATA', 'fixed_data.bin'), 'rb').read()
        except FileNotFoundError:
            fixed = b''
        pos = 0
        while pos < len(fixed):
            addr, size = struct.unpack_from('<QQ', fixed, pos)
            self.mu.mem_write(addr, fixed[pos + 16:pos + 16 + size])
            pos += 16 + size
        self.mu.mem_map(STACK - STACK_SIZE, STACK_SIZE, UC_PROT_ALL)
        self.mu.mem_map(SENTINEL, 0x1000, UC_PROT_ALL)
        self.mu.mem_write(SENTINEL, b'\xf4')  # hlt
        self.mu.mem_map(HEAP, HEAP_SIZE, UC_PROT_ALL)
        self.heap_top = HEAP

    def alloc(self, size, data=None):
        addr = self.heap_top
        self.heap_top += (size + 0xF) & ~0xF
        self.mu.mem_write(addr, data if data is not None else bytes(size))
        return addr

    def call(self, fn, *args, limit=50_000_000):
        regs = [UC_X86_REG_RDI, UC_X86_REG_RSI, UC_X86_REG_RDX, UC_X86_REG_RCX, UC_X86_REG_R8, UC_X86_REG_R9]
        for reg, value in zip(regs, args):
            self.mu.reg_write(reg, value & 0xFFFFFFFFFFFFFFFF)
        sp = STACK - 0x1000
        self.mu.mem_write(sp, struct.pack('<Q', SENTINEL))
        self.mu.reg_write(UC_X86_REG_RSP, sp)
        self.mu.emu_start(fn, SENTINEL, count=limit)
        return self.mu.reg_read(UC_X86_REG_RAX), self.mu.reg_read(UC_X86_REG_RDX)
