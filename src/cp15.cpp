#include "cp15.h"

namespace hydra {

namespace {

using Dynarmic::A32::CoprocReg;

unsigned index(CoprocReg reg) { return static_cast<unsigned>(reg); }

// Registers that are plain storage: written values read back unchanged.
bool isStorage(std::uint32_t k) {
    switch (k) {
    case Cp15::key(0, 1, 0, 0):   // SCTLR
    case Cp15::key(0, 1, 0, 1):   // ACTLR
    case Cp15::key(0, 1, 0, 2):   // CPACR
    case Cp15::key(0, 2, 0, 0):   // TTBR0
    case Cp15::key(0, 2, 0, 1):   // TTBR1
    case Cp15::key(0, 2, 0, 2):   // TTBCR
    case Cp15::key(0, 3, 0, 0):   // DACR
    case Cp15::key(0, 5, 0, 0):   // DFSR
    case Cp15::key(0, 5, 0, 1):   // IFSR
    case Cp15::key(0, 6, 0, 0):   // DFAR
    case Cp15::key(0, 6, 0, 2):   // IFAR
    case Cp15::key(0, 7, 4, 0):   // PAR
    case Cp15::key(0, 10, 2, 0):  // PRRR
    case Cp15::key(0, 10, 2, 1):  // NMRR
    case Cp15::key(0, 12, 0, 0):  // VBAR
    case Cp15::key(0, 13, 0, 1):  // CONTEXTIDR
    case Cp15::key(0, 13, 0, 2):  // TPIDRURW
    case Cp15::key(0, 13, 0, 3):  // TPIDRURO
    case Cp15::key(0, 13, 0, 4):  // TPIDRPRW
    case Cp15::key(2, 0, 0, 0):   // CSSELR
        return true;
    default:
        return false;
    }
}

}  // namespace

Cp15::Cp15(Hooks hooks) : hooks_(std::move(hooks)) {
    regs_[kSctlr] = 0x00C50078;  // Cortex-A5 reset value: MMU and caches off, low vectors
    regs_[kTtbr0] = 0;
    regs_[kVbar] = 0;
}

std::uint32_t Cp15::read(std::uint32_t k) {
    switch (k) {
    case key(0, 0, 0, 0): return 0x410FC051;  // MIDR: Cortex-A5 r0p1
    case key(0, 0, 0, 1): return 0x83338003;  // CTR
    case key(0, 0, 0, 3): return 0;           // TLBTR: unified TLB
    case key(0, 0, 0, 5): return 0x80000000;  // MPIDR: CPU 0
    case key(1, 0, 0, 0): return 0x701FE019;  // CCSIDR: 32 KiB, 4 ways, 32 byte lines
    case key(1, 0, 0, 1): return 0x09200003;  // CLIDR: separate L1 I and D
    case key(1, 0, 0, 7): return 0;           // AIDR
    }
    if (k >> 8 == 0 && (k >> 4 & 0xF) >= 1 && (k >> 4 & 0xF) <= 2)
        return 0;  // ID_PFRx / ID_MMFRx / ID_ISARx: report no optional features
    if (const auto it = regs_.find(k); it != regs_.end())
        return it->second;
    if (!isStorage(k))
        ++unknown_[k];
    return 0;
}

void Cp15::write(std::uint32_t k, std::uint32_t value) {
    const unsigned crn = k >> 8 & 0xF, crm = k >> 4 & 0xF, opc2 = k & 7;
    if (crn == 7 || crn == 8) {  // cache, branch predictor and TLB maintenance
        const bool invalidatesInstructions = crn == 7 && (crm == 5 || (crm == 1 && opc2 == 0));
        if (invalidatesInstructions && hooks_.instructionCacheInvalidated)
            hooks_.instructionCacheInvalidated();
        if (k != key(0, 7, 4, 0))
            return;
    }
    if (!isStorage(k))
        ++unknown_[k];
    regs_[k] = value;
    if ((k == kSctlr || k == kTtbr0) && hooks_.mmuChanged)
        hooks_.mmuChanged(regs_[kSctlr], regs_[kTtbr0]);
}

Cp15::Callback Cp15::callbackFor(std::uint32_t k, bool isWrite) {
    Slot& slot = slots_.emplace_back(Slot{this, k});
    return Callback{isWrite ? &writeThunk : &readThunk, &slot};
}

std::uint64_t Cp15::readThunk(void* slot, std::uint32_t, std::uint32_t) {
    auto* s = static_cast<Slot*>(slot);
    return s->self->read(s->key);
}

std::uint64_t Cp15::writeThunk(void* slot, std::uint32_t value, std::uint32_t) {
    auto* s = static_cast<Slot*>(slot);
    s->self->write(s->key, value);
    return 0;
}

std::optional<Cp15::Callback> Cp15::CompileInternalOperation(bool, unsigned, CoprocReg, CoprocReg, CoprocReg, unsigned) {
    return std::nullopt;
}

Cp15::CallbackOrAccessOneWord Cp15::CompileSendOneWord(bool two, unsigned opc1, CoprocReg CRn, CoprocReg CRm, unsigned opc2) {
    if (two)
        return std::monostate{};
    return callbackFor(key(opc1, index(CRn), index(CRm), opc2), true);
}

Cp15::CallbackOrAccessOneWord Cp15::CompileGetOneWord(bool two, unsigned opc1, CoprocReg CRn, CoprocReg CRm, unsigned opc2) {
    if (two)
        return std::monostate{};
    return callbackFor(key(opc1, index(CRn), index(CRm), opc2), false);
}

Cp15::CallbackOrAccessTwoWords Cp15::CompileSendTwoWords(bool, unsigned, CoprocReg) { return std::monostate{}; }
Cp15::CallbackOrAccessTwoWords Cp15::CompileGetTwoWords(bool, unsigned, CoprocReg) { return std::monostate{}; }

std::optional<Cp15::Callback> Cp15::CompileLoadWords(bool, bool, CoprocReg, std::optional<std::uint8_t>) {
    return std::nullopt;
}

std::optional<Cp15::Callback> Cp15::CompileStoreWords(bool, bool, CoprocReg, std::optional<std::uint8_t>) {
    return std::nullopt;
}

}  // namespace hydra
