#include "arm_system.h"

#include <bit>

namespace hydra::arm {

namespace {

bool conditionPassed(std::uint32_t cond, std::uint32_t cpsr) {
    const bool n = cpsr >> 31 & 1, z = cpsr >> 30 & 1, c = cpsr >> 29 & 1, v = cpsr >> 28 & 1;
    switch (cond) {
    case 0x0: return z;
    case 0x1: return !z;
    case 0x2: return c;
    case 0x3: return !c;
    case 0x4: return n;
    case 0x5: return !n;
    case 0x6: return v;
    case 0x7: return !v;
    case 0x8: return c && !z;
    case 0x9: return !c || z;
    case 0xA: return n == v;
    case 0xB: return n != v;
    case 0xC: return !z && n == v;
    case 0xD: return z || n != v;
    default: return true;
    }
}

bool isPrivileged(std::uint32_t mode) { return mode != USR; }

}  // namespace

bool SystemState::isSystemInstruction(std::uint32_t inst) {
    if (inst >> 28 == 0xF) {
        return (inst & 0xFFF1FE20) == 0xF1000000      // CPS
               || (inst & 0xFE5FFFE0) == 0xF84D0500   // SRS
               || (inst & 0xFE50FFFF) == 0xF8100A00;  // RFE
    }
    if ((inst & 0x0FBF0FFF) == 0x010F0000)  // MRS Rd, CPSR/SPSR
        return true;
    if ((inst & 0x0FB0F000) == 0x0320F000)  // MSR imm (mask 0 with R=0 is a hint)
        return (inst & 0x000F0000) != 0 || (inst & (1u << 22)) != 0;
    if ((inst & 0x0FB0FFF0) == 0x0120F000)  // MSR reg
        return true;
    if ((inst & 0x0E400000) == 0x08400000)  // LDM/STM with ^
        return true;
    if ((inst & 0x0C10F000) == 0x0010F000) {  // data processing, S=1, Rd=PC
        const bool multiplyOrExtraLoad = (inst & 0x02000090) == 0x90;
        const std::uint32_t opcode = inst >> 21 & 0xF;
        const bool compare = opcode >= 0x8 && opcode <= 0xB;
        return !multiplyOrExtraLoad && !compare;
    }
    if ((inst & 0x0FE00FFF) == 0x0EE00A10)  // VMRS/VMSR; FPSCR (reg 1) stays with Dynarmic
        return (inst >> 16 & 0xF) != 1;
    return false;
}

int SystemState::bankIndex(std::uint32_t mode) {
    switch (mode) {
    case FIQ: return 1;
    case IRQ: return 2;
    case SVC: return 3;
    case ABT: return 4;
    case UND: return 5;
    case MON: return 6;
    default: return 0;  // USR, SYS and anything unexpected share the user bank
    }
}

void SystemState::switchBanks(std::uint32_t from, std::uint32_t to) {
    auto& r = cpu_.regs();
    const int oldBank = bankIndex(from), newBank = bankIndex(to);
    if (oldBank != newBank) {
        banks_[oldBank].r13 = r[13];
        banks_[oldBank].r14 = r[14];
        r[13] = banks_[newBank].r13;
        r[14] = banks_[newBank].r14;
    }
    if ((from == FIQ) != (to == FIQ)) {
        auto& save = from == FIQ ? fiqHigh_ : usrHigh_;
        const auto& restore = to == FIQ ? fiqHigh_ : usrHigh_;
        for (int i = 0; i < 5; ++i) {
            save[i] = r[8 + i];
            r[8 + i] = restore[i];
        }
    }
}

void SystemState::writeCpsr(std::uint32_t value) {
    const std::uint32_t from = mode(), to = value & psr::M;
    if (from != to)
        switchBanks(from, to);
    cpu_.setCpsr(value);
}

std::uint32_t SystemState::spsr() {
    const int bank = bankIndex(mode());
    return bank == 0 ? cpu_.cpsr() : banks_[bank].spsr;  // no SPSR in USR/SYS: UNPREDICTABLE
}

void SystemState::setSpsr(std::uint32_t value) {
    if (const int bank = bankIndex(mode()); bank != 0)
        banks_[bank].spsr = value;
}

std::uint32_t SystemState::userReg(int n) {
    const std::uint32_t current = mode();
    if (n >= 8 && n <= 12 && current == FIQ)
        return usrHigh_[n - 8];
    if (n >= 13 && n <= 14 && bankIndex(current) != 0)
        return n == 13 ? banks_[0].r13 : banks_[0].r14;
    return cpu_.regs()[n];
}

void SystemState::setUserReg(int n, std::uint32_t value) {
    const std::uint32_t current = mode();
    if (n >= 8 && n <= 12 && current == FIQ)
        usrHigh_[n - 8] = value;
    else if (n >= 13 && n <= 14 && bankIndex(current) != 0)
        (n == 13 ? banks_[0].r13 : banks_[0].r14) = value;
    else
        cpu_.regs()[n] = value;
}

std::uint32_t SystemState::bankedSp(std::uint32_t target) {
    return bankIndex(target) == bankIndex(mode()) ? cpu_.regs()[13] : banks_[bankIndex(target)].r13;
}

void SystemState::setBankedSp(std::uint32_t target, std::uint32_t value) {
    if (bankIndex(target) == bankIndex(mode()))
        cpu_.regs()[13] = value;
    else
        banks_[bankIndex(target)].r13 = value;
}

void SystemState::exceptionReturn(std::uint32_t newPc, std::uint32_t newCpsr) {
    writeCpsr(newCpsr);
    cpu_.regs()[15] = newPc & (newCpsr & psr::T ? ~1u : ~3u);
}

void SystemState::enterException(ExceptionKind kind, std::uint32_t returnAddress) {
    struct Entry {
        std::uint32_t mode, offset;
        bool maskFiq, maskAbort;
    };
    Entry entry{};
    switch (kind) {
    case ExceptionKind::Reset: entry = {SVC, 0x00, true, true}; break;
    case ExceptionKind::Undefined: entry = {UND, 0x04, false, false}; break;
    case ExceptionKind::SupervisorCall: entry = {SVC, 0x08, false, false}; break;
    case ExceptionKind::PrefetchAbort: entry = {ABT, 0x0C, false, true}; break;
    case ExceptionKind::DataAbort: entry = {ABT, 0x10, false, true}; break;
    case ExceptionKind::Irq: entry = {IRQ, 0x18, false, true}; break;
    case ExceptionKind::Fiq: entry = {FIQ, 0x1C, true, true}; break;
    }

    const std::uint32_t oldCpsr = cpu_.cpsr();
    std::uint32_t newCpsr = oldCpsr & ~(psr::M | psr::T | psr::E | psr::J | psr::IT);
    newCpsr |= entry.mode | psr::I;
    if (entry.maskFiq)
        newCpsr |= psr::F;
    if (entry.maskAbort)
        newCpsr |= psr::A;
    if (cpu_.exceptionsInThumb())
        newCpsr |= psr::T;
    if (cpu_.exceptionsBigEndian())
        newCpsr |= psr::E;

    writeCpsr(newCpsr);
    banks_[bankIndex(entry.mode)].spsr = oldCpsr;
    cpu_.regs()[14] = returnAddress;
    cpu_.regs()[15] = cpu_.vectorBase() + entry.offset;
}

SystemState::Result SystemState::execute(std::uint32_t inst, std::uint32_t pc) {
    auto& r = cpu_.regs();

    if (inst >> 28 == 0xF) {
        if ((inst & 0xFFF1FE20) == 0xF1000000) {  // CPS
            if (!isPrivileged(mode()))
                return Result::Done;
            std::uint32_t cpsr = cpu_.cpsr();
            const std::uint32_t imod = inst >> 18 & 3, flags = inst & (psr::A | psr::I | psr::F);
            if (imod == 2)
                cpsr &= ~flags;
            else if (imod == 3)
                cpsr |= flags;
            if (inst & (1u << 17))
                cpsr = (cpsr & ~psr::M) | (inst & psr::M);
            writeCpsr(cpsr);
            return Result::Done;
        }

        const bool p = inst >> 24 & 1, u = inst >> 23 & 1, w = inst >> 21 & 1;
        if ((inst & 0xFE5FFFE0) == 0xF84D0500) {  // SRS{mode}
            const std::uint32_t target = inst & psr::M;
            const std::uint32_t sp = bankedSp(target);
            const std::uint32_t addr = u ? (p ? sp + 4 : sp) : (p ? sp - 8 : sp - 4);
            cpu_.write32(addr, r[14]);
            cpu_.write32(addr + 4, spsr());
            if (w)
                setBankedSp(target, u ? sp + 8 : sp - 8);
            return Result::Done;
        }
        if ((inst & 0xFE50FFFF) == 0xF8100A00) {  // RFE
            const std::uint32_t n = inst >> 16 & 0xF, base = r[n];
            const std::uint32_t addr = u ? (p ? base + 4 : base) : (p ? base - 8 : base - 4);
            const std::uint32_t newPc = cpu_.read32(addr), newCpsr = cpu_.read32(addr + 4);
            if (w)
                r[n] = u ? base + 8 : base - 8;
            exceptionReturn(newPc, newCpsr);
            return Result::Done;
        }
        return Result::Unsupported;
    }

    if (!conditionPassed(inst >> 28, cpu_.cpsr()))
        return Result::Done;

    if ((inst & 0x0FBF0FFF) == 0x010F0000) {  // MRS
        r[inst >> 12 & 0xF] = inst & (1u << 22) ? spsr() : cpu_.cpsr();
        return Result::Done;
    }
    if ((inst & 0x0FB0F000) == 0x0320F000) {  // MSR imm
        const std::uint32_t rotate = (inst >> 8 & 0xF) * 2;
        return msr(inst, std::rotr(inst & 0xFFu, static_cast<int>(rotate)));
    }
    if ((inst & 0x0FB0FFF0) == 0x0120F000)  // MSR reg
        return msr(inst, r[inst & 0xF]);
    if ((inst & 0x0E400000) == 0x08400000)
        return loadStoreMultipleUser(inst, pc);
    if ((inst & 0x0C10F000) == 0x0010F000)
        return dataProcessingReturn(inst, pc);
    if ((inst & 0x0FE00FFF) == 0x0EE00A10)
        return vfpSystemRegister(inst);
    return Result::Unsupported;
}

SystemState::Result SystemState::msr(std::uint32_t inst, std::uint32_t value) {
    const std::uint32_t fields = inst >> 16 & 0xF;
    std::uint32_t mask = 0;
    for (int byte = 0; byte < 4; ++byte)
        if (fields & (1u << byte))
            mask |= 0xFFu << (8 * byte);

    if (inst & (1u << 22)) {  // SPSR: every masked bit is writable
        setSpsr((spsr() & ~mask) | (value & mask));
        return Result::Done;
    }

    // CPSR: execution state bits (T, J, IT) cannot be written by MSR; mode,
    // interrupt masks and A are writable only in privileged modes.
    std::uint32_t writable = 0xF80F0200;  // N Z C V Q, GE, E
    if (isPrivileged(mode()))
        writable |= psr::M | psr::F | psr::I | psr::A;
    mask &= writable;
    writeCpsr((cpu_.cpsr() & ~mask) | (value & mask));
    return Result::Done;
}

// SUBS PC, LR, #imm / MOVS PC, LR and friends: compute the result, then
// return from the exception by restoring CPSR from SPSR.
SystemState::Result SystemState::dataProcessingReturn(std::uint32_t inst, std::uint32_t pc) {
    auto& r = cpu_.regs();
    const auto reg = [&](std::uint32_t n) { return n == 15 ? pc + 8 : r[n]; };
    const bool carryIn = cpu_.cpsr() & psr::C;

    std::uint32_t operand2;
    if (inst & (1u << 25)) {
        operand2 = std::rotr(inst & 0xFFu, static_cast<int>((inst >> 8 & 0xF) * 2));
    } else {
        const std::uint32_t rm = reg(inst & 0xF), type = inst >> 5 & 3;
        const bool byRegister = inst & (1u << 4);
        std::uint32_t amount = byRegister ? r[inst >> 8 & 0xF] & 0xFF : inst >> 7 & 0x1F;
        if (!byRegister && amount == 0 && (type == 1 || type == 2))
            amount = 32;  // LSR #0 / ASR #0 encode a shift by 32
        switch (type) {
        case 0: operand2 = amount >= 32 ? 0 : rm << amount; break;
        case 1: operand2 = amount >= 32 ? 0 : rm >> amount; break;
        case 2: operand2 = static_cast<std::uint32_t>(static_cast<std::int32_t>(rm) >> (amount >= 32 ? 31 : amount)); break;
        default:
            if (!byRegister && amount == 0)
                operand2 = (carryIn ? 0x80000000u : 0) | rm >> 1;  // RRX
            else
                operand2 = std::rotr(rm, static_cast<int>(amount & 31));
        }
    }

    const std::uint32_t rn = reg(inst >> 16 & 0xF), c = carryIn ? 1 : 0;
    std::uint32_t result;
    switch (inst >> 21 & 0xF) {
    case 0x0: result = rn & operand2; break;
    case 0x1: result = rn ^ operand2; break;
    case 0x2: result = rn - operand2; break;
    case 0x3: result = operand2 - rn; break;
    case 0x4: result = rn + operand2; break;
    case 0x5: result = rn + operand2 + c; break;
    case 0x6: result = rn - operand2 - (1 - c); break;
    case 0x7: result = operand2 - rn - (1 - c); break;
    case 0xC: result = rn | operand2; break;
    case 0xD: result = operand2; break;
    case 0xE: result = rn & ~operand2; break;
    case 0xF: result = ~operand2; break;
    default: return Result::Unsupported;  // TST/TEQ/CMP/CMN are never claimed
    }
    exceptionReturn(result, spsr());
    return Result::Done;
}

// LDM/STM with the ^ suffix: user-bank transfers, or LDM exception return when PC is loaded.
SystemState::Result SystemState::loadStoreMultipleUser(std::uint32_t inst, std::uint32_t pc) {
    auto& r = cpu_.regs();
    const bool p = inst >> 24 & 1, u = inst >> 23 & 1, w = inst >> 21 & 1, load = inst >> 20 & 1;
    const std::uint32_t n = inst >> 16 & 0xF, list = inst & 0xFFFF;
    const std::uint32_t count = std::popcount(list), base = r[n];
    std::uint32_t addr = u ? (p ? base + 4 : base) : (p ? base - 4 * count : base - 4 * count + 4);

    if (!load) {
        for (int i = 0; i < 16; ++i)
            if (list >> i & 1) {
                cpu_.write32(addr, i == 15 ? pc + 8 : userReg(i));
                addr += 4;
            }
        return Result::Done;
    }

    if (!(list & 0x8000)) {
        for (int i = 0; i < 15; ++i)
            if (list >> i & 1) {
                setUserReg(i, cpu_.read32(addr));
                addr += 4;
            }
        return Result::Done;
    }

    for (int i = 0; i < 15; ++i)
        if (list >> i & 1) {
            r[i] = cpu_.read32(addr);
            addr += 4;
        }
    const std::uint32_t newPc = cpu_.read32(addr);
    if (w && !(list >> n & 1))
        r[n] = u ? base + 4 * count : base - 4 * count;
    exceptionReturn(newPc, spsr());
    return Result::Done;
}

// VMRS/VMSR for FPSID, MVFR0/1 and FPEXC. FPSCR is handled by Dynarmic itself.
SystemState::Result SystemState::vfpSystemRegister(std::uint32_t inst) {
    auto& r = cpu_.regs();
    const bool toArm = inst >> 20 & 1;
    const std::uint32_t t = inst >> 12 & 0xF;
    std::uint32_t* target = nullptr;
    std::uint32_t constant = 0;
    switch (inst >> 16 & 0xF) {
    case 0x0: constant = 0x41023051; break;  // FPSID: ARM VFPv3+ subarchitecture
    case 0x6: constant = 0x11000011; break;  // MVFR1
    case 0x7: constant = 0x10110221; break;  // MVFR0: VFPv4-D16
    case 0x8: target = &fpexc_; break;
    default: return Result::Unsupported;
    }
    if (toArm)
        r[t] = target ? *target : constant;
    else if (target)
        *target = r[t];
    return Result::Done;
}

}  // namespace hydra::arm
