#include "cortex_m3.h"

#include <cstdlib>

#include <cstdio>

#include <dynarmic/frontend/A32/a32_ir_emitter.h>
#include <dynarmic/frontend/A32/a32_location_descriptor.h>

#include <utility>

#include <algorithm>
#include <cstring>
#include <format>

namespace hydra {

using Dynarmic::A32::Exception;

namespace {

constexpr std::uint32_t kThumb = 1u << 5;
constexpr std::uint32_t kItBits = 0x0600FC00;
constexpr std::uint32_t kStackedFlags = 0xFE00FC00;  // N Z C V Q, IT, (T is bit 24 in xPSR)

}  // namespace

CortexM3::CortexM3(MmioBus& bus)
    : bus_(bus), flash_(kFlashSize, 0xFF), sram_(kSramSize),
      pageTable_(std::make_unique<std::array<std::uint8_t*, Dynarmic::A32::UserConfig::NUM_PAGE_TABLE_ENTRIES>>()) {
    pageTable_->fill(nullptr);
    for (std::uint32_t offset = 0; offset < kSramSize; offset += 0x1000)
        (*pageTable_)[(kSramBase + offset) >> 12] = sram_.data() + offset;

    Dynarmic::A32::UserConfig config;
    config.callbacks = this;
    config.arch_version = Dynarmic::A32::ArchVersion::v7;
    config.page_table = pageTable_.get();
    config.define_unpredictable_behaviour = true;
    // For stopAfterAccess(): linked blocks only check the cycle budget.
    config.check_halt_on_memory_access = true;
    jit_ = std::make_unique<Dynarmic::A32::Jit>(config);
}

CortexM3::~CortexM3() = default;

void CortexM3::loadFlash(std::uint32_t offset, const std::vector<std::uint8_t>& data) {
    std::copy_n(data.begin(), std::min<std::size_t>(data.size(), flash_.size() - offset), flash_.begin() + offset);
    jit_->ClearCache();
}

void CortexM3::reset(std::uint32_t vectors) {
    jit_->Reset();
    jit_->Regs().fill(0);
    enabled_.fill(false);
    pending_.fill(false);
    active_.fill(false);
    priority_.fill(0);
    vtor_ = vectors & ~0x7Fu;
    primask_ = faultmask_ = basepri_ = control_ = ipsr_ = 0;
    usingPsp_ = false;
    psp_ = 0;
    systCsr_ = systReload_ = systCurrent_ = 0;
    sleeping_ = false;
    fatal_.clear();
    msp_ = read(vectors, 4);
    regs()[13] = msp_;
    regs()[14] = 0xFFFFFFFF;
    regs()[15] = read(vectors + 4, 4) & ~1u;
    jit_->SetCpsr(kThumb | 0x13);
}

// --- memory -------------------------------------------------------------------

std::uint32_t CortexM3::read(std::uint32_t a, int size) {
    std::uint32_t value = 0;
    if (a < kFlashSize && a + size <= kFlashSize) {
        std::memcpy(&value, flash_.data() + a, size);
        return value;
    }
    if (a - kFlashBase < kFlashSize && a - kFlashBase + size <= kFlashSize) {
        std::memcpy(&value, flash_.data() + (a - kFlashBase), size);
        return value;
    }
    if (a - kSramBase < kSramSize && a - kSramBase + size <= kSramSize) {
        std::memcpy(&value, sram_.data() + (a - kSramBase), size);
        return value;
    }
    if (a - 0x22000000 < kSramSize * 32) {  // SRAM bit-band
        const std::uint32_t bit = (a - 0x22000000) >> 2;
        return sram_[bit >> 3] >> (bit & 7) & 1;
    }
    if ((a & 0xFE000000) == 0x42000000) {  // peripheral bit-band
        const std::uint32_t word = 0x40000000 | ((a - 0x42000000) >> 5 & ~3u);
        return bus_.read(word, 4) >> ((a >> 2) & 31) & 1;
    }
    if ((a & 0xFFFFF000) == 0xE000E000) {
        value = scsRead(a & 0xFFC) >> (8 * (a & 3));
        return size == 4 ? value : value & ((1u << (8 * size)) - 1);
    }
    if (a < 0xE0000000) {
        bus_.log().currentPc = regs()[15];
        return bus_.read(a, size);
    }
    return 0;
}

void CortexM3::write(std::uint32_t a, std::uint32_t value, int size) {
    if (a - kSramBase < kSramSize && a - kSramBase + size <= kSramSize) {
        std::memcpy(sram_.data() + (a - kSramBase), &value, size);
        return;
    }
    if (a < kFlashSize || a - kFlashBase < kFlashSize)
        return;  // flash programming is not modelled
    if (a - 0x22000000 < kSramSize * 32) {
        const std::uint32_t bit = (a - 0x22000000) >> 2;
        std::uint8_t& byte = sram_[bit >> 3];
        byte = static_cast<std::uint8_t>((byte & ~(1u << (bit & 7))) | (value & 1) << (bit & 7));
        return;
    }
    if ((a & 0xFE000000) == 0x42000000) {
        const std::uint32_t word = 0x40000000 | ((a - 0x42000000) >> 5 & ~3u);
        const std::uint32_t bit = (a >> 2) & 31;
        bus_.log().currentPc = regs()[15];
        bus_.write(word, (bus_.read(word, 4) & ~(1u << bit)) | (value & 1) << bit, 4);
        return;
    }
    if ((a & 0xFFFFF000) == 0xE000E000) {
        if (size == 4)
            scsWrite(a & 0xFFC, value);
        else {
            const std::uint32_t shift = 8 * (a & 3), mask = ((1u << (8 * size)) - 1) << shift;
            scsWrite(a & 0xFFC, (scsRead(a & 0xFFC) & ~mask) | (value << shift & mask));
        }
        return;
    }
    if (a < 0xE0000000) {
        bus_.log().currentPc = regs()[15];
        bus_.write(a, value, size);
    }
}

bool CortexM3::writeExclusive(std::uint32_t a, std::uint32_t value, std::uint32_t expected, int size) {
    if (read(a, size) != expected)
        return false;
    write(a, value, size);
    return true;
}

std::optional<std::uint32_t> CortexM3::MemoryReadCode(std::uint32_t a) {
    if (a >= 0xFFFFFFF0)
        return std::nullopt;  // EXC_RETURN: handled as an exception return
    if (a < kFlashSize || a - kFlashBase < kFlashSize || a - kSramBase < kSramSize)
        return read(a, 4);
    return std::nullopt;
}

// --- system instructions ------------------------------------------------------

bool CortexM3::IsSystemInstruction(bool is_thumb, std::uint32_t insn) {
    if (!is_thumb)
        return false;
    if (insn <= 0xFFFF)
        return (insn & 0xFFE8) == 0xB660      // CPSIE/CPSID
               || (insn & 0xFFEF) == 0xBF20;  // WFE/WFI
    const std::uint32_t hw1 = insn >> 16, hw2 = insn & 0xFFFF;
    return (hw1 == 0xF3EF && (hw2 & 0xF000) == 0x8000)             // MRS
           || ((hw1 & 0xFFF0) == 0xF380 && (hw2 & 0xF300) == 0x8000);  // MSR
}

std::uint32_t CortexM3::xpsr() {
    const std::uint32_t cpsr = jit_->Cpsr();
    return (cpsr & kStackedFlags) | 1u << 24 | (ipsr_ & 0x1FF);
}

void CortexM3::setActiveStack(bool process) {
    if (process == usingPsp_)
        return;
    std::uint32_t& sp = regs()[13];
    if (usingPsp_)
        psp_ = sp;
    else
        msp_ = sp;
    sp = process ? psp_ : msp_;
    usingPsp_ = process;
}

void CortexM3::executeSystemInstruction(std::uint32_t pc) {
    auto& r = regs();
    const std::uint32_t hw1 = read(pc, 2);
    if ((hw1 & 0xFFE8) == 0xB660) {  // CPS
        const std::uint32_t disable = hw1 >> 4 & 1;
        if (hw1 & 2)
            primask_ = disable;
        if (hw1 & 1)
            faultmask_ = disable;
        return;
    }
    if ((hw1 & 0xFFEF) == 0xBF20) {  // WFE/WFI: sleep until an exception can be taken
        if (pendingException() == 0)
            sleeping_ = true;
        return;
    }
    const std::uint32_t hw2 = read(pc + 2, 2);
    const std::uint32_t sysm = hw2 & 0xFF;
    const bool privileged = ipsr_ != 0 || !(control_ & 1);
    if (hw1 == 0xF3EF) {  // MRS Rd, <spec_reg>
        std::uint32_t value = 0;
        if (sysm <= 7) {
            if (!(sysm & 4))
                value |= jit_->Cpsr() & 0xF8000000;  // APSR
            if (sysm & 1)
                value |= ipsr_ & 0x1FF;  // IPSR
        } else if (sysm == 8) {
            value = usingPsp_ ? msp_ : r[13];
        } else if (sysm == 9) {
            value = usingPsp_ ? r[13] : psp_;
        } else if (sysm == 16) {
            value = primask_;
        } else if (sysm == 17 || sysm == 18) {
            value = basepri_;
        } else if (sysm == 19) {
            value = faultmask_;
        } else if (sysm == 20) {
            value = (control_ & 1) | (usingPsp_ && ipsr_ == 0 ? 2 : (control_ & 2));
        }
        r[hw2 >> 8 & 0xF] = value;
        return;
    }
    // MSR <spec_reg>, Rn
    const std::uint32_t value = r[hw1 & 0xF];
    if (sysm <= 3) {
        if (hw2 >> 11 & 1)  // mask: write NZCVQ
            jit_->SetCpsr((jit_->Cpsr() & ~0xF8000000u) | (value & 0xF8000000));
    } else if (!privileged) {
        return;
    } else if (sysm == 8) {
        (usingPsp_ ? msp_ : r[13]) = value & ~3u;
    } else if (sysm == 9) {
        (usingPsp_ ? r[13] : psp_) = value & ~3u;
    } else if (sysm == 16) {
        primask_ = value & 1;
    } else if (sysm == 17) {
        basepri_ = value & 0xFF;
    } else if (sysm == 18) {
        if ((value & 0xFF) && ((value & 0xFF) < basepri_ || basepri_ == 0))
            basepri_ = value & 0xFF;
    } else if (sysm == 19) {
        if (executionPriority() > -1)
            faultmask_ = value & 1;
    } else if (sysm == 20) {
        control_ = value & 3;
        if (ipsr_ == 0)
            setActiveStack(control_ & 2);
    }
}

void CortexM3::CallSVC(std::uint32_t) {
    stop_ = Stop::SystemInstruction;  // stopPc_ ~0: take SVCall before running on
    stopPc_ = ~0u;
    jit_->HaltExecution(Dynarmic::HaltReason::UserDefined1);
}

void CortexM3::ExceptionRaised(std::uint32_t pc, Exception exception) {
    if (exception == Exception::SystemInstruction) {
        stop_ = Stop::SystemInstruction;
        stopPc_ = pc;
    } else if (exception == Exception::NoExecuteFault && pc >= 0xFFFFFFF0) {
        stop_ = Stop::ExceptionReturn;
        stopPc_ = pc | 1;
    } else {
        stop_ = Stop::Fatal;
        fail(std::format("exception {} at {:08x} ({:04x} {:04x})", static_cast<int>(exception), pc, read(pc, 2), read(pc + 2, 2)));
    }
    jit_->HaltExecution(Dynarmic::HaltReason::UserDefined1);
}

void CortexM3::InterpreterFallback(std::uint32_t pc, std::size_t) {
    fail(std::format("interpreter fallback at {:08x} ({:04x} {:04x})", pc, read(pc, 2), read(pc + 2, 2)));
    stop_ = Stop::Fatal;
    jit_->HaltExecution(Dynarmic::HaltReason::UserDefined1);
}

void CortexM3::fail(std::string message) {
    if (fatal_.empty())
        fatal_ = std::move(message);
}

void CortexM3::AddTicks(std::uint64_t ticks) {
    now_ += ticks;
    ticksRemaining_ = ticks >= ticksRemaining_ ? 0 : ticksRemaining_ - ticks;
}

// --- exceptions -----------------------------------------------------------------

int CortexM3::priority(int exception) const {
    if (exception >= 1 && exception <= 3)
        return exception - 4;  // reset -3, NMI -2, HardFault -1
    const int groupMask = (0x1FE << priGroup_) & 0xFF;
    return priority_[exception] & groupMask;
}

int CortexM3::executionPriority() const {
    int best = 256;
    for (int e = 2; e < kExceptions; ++e)
        if (active_[e])
            best = std::min(best, priority(e));
    if (basepri_ != 0)
        best = std::min(best, static_cast<int>(basepri_ & ((0x1FE << priGroup_) & 0xFF)));
    if (primask_)
        best = std::min(best, 0);
    if (faultmask_)
        best = std::min(best, -1);
    return best;
}

int CortexM3::pendingException() const {
    if (std::memchr(pending_.data(), true, pending_.size()) == nullptr)
        return 0;
    int best = 0, bestPriority = 1000;
    for (int e = 2; e < kExceptions; ++e) {
        if (!pending_[e] || (e >= 16 && !enabled_[e]))
            continue;
        const int p = priority(e);
        if (p < bestPriority) {
            bestPriority = p;
            best = e;
        }
    }
    return best != 0 && bestPriority < executionPriority() ? best : 0;
}

void CortexM3::enterException(int exception, std::uint32_t returnAddress) {
    ++exceptionsTaken_;
    auto& r = regs();
    const std::uint32_t sp = r[13];
    const bool realign = (ccr_ & 0x200) && (sp & 4);
    const std::uint32_t frame = (sp - 32) & (realign ? ~4u : ~0u);
    const std::uint32_t stackedXpsr = (xpsr() & ~0x200u) | (realign ? 0x200 : 0);
    const std::uint32_t values[8] = {r[0], r[1], r[2], r[3], r[12], r[14], returnAddress, stackedXpsr};
    for (int i = 0; i < 8; ++i)
        write(frame + 4 * i, values[i], 4);
    r[13] = frame;

    const std::uint32_t excReturn = ipsr_ != 0 ? 0xFFFFFFF1 : (usingPsp_ ? 0xFFFFFFFD : 0xFFFFFFF9);
    setActiveStack(false);
    r[14] = excReturn;
    pending_[exception] = false;
    active_[exception] = true;
    ipsr_ = static_cast<std::uint32_t>(exception);
    r[15] = read(vtor_ + 4 * static_cast<std::uint32_t>(exception), 4) & ~1u;
    jit_->SetCpsr((jit_->Cpsr() & ~kItBits) | kThumb);
}

void CortexM3::exceptionReturn(std::uint32_t excReturn) {
    const std::uint32_t kind = excReturn & 0xF;
    if (kind != 1 && kind != 9 && kind != 0xD) {
        fail(std::format("bad EXC_RETURN {:08x}", excReturn));
        return;
    }
    if (ipsr_ < kExceptions)
        active_[ipsr_] = false;

    const bool toProcess = kind == 0xD;
    setActiveStack(toProcess);
    auto& r = regs();
    const std::uint32_t frame = r[13];
    std::uint32_t values[8];
    for (int i = 0; i < 8; ++i)
        values[i] = read(frame + 4 * i, 4);
    r[13] = frame + 32 + ((values[7] >> 9 & 1) ? 4 : 0);
    r[0] = values[0];
    r[1] = values[1];
    r[2] = values[2];
    r[3] = values[3];
    r[12] = values[4];
    r[14] = values[5];
    r[15] = values[6] & ~1u;
    ipsr_ = kind == 1 ? values[7] & 0x1FF : 0;
    control_ = (control_ & ~2u) | (toProcess ? 2 : 0);
    jit_->SetCpsr((values[7] & kStackedFlags) | (jit_->Cpsr() & 0x01FF03DF) | kThumb);
}

// --- system control space -------------------------------------------------------

std::uint32_t CortexM3::scsRead(std::uint32_t offset) {
    const auto bits = [&](const std::array<bool, kExceptions>& set, std::uint32_t base) {
        std::uint32_t value = 0;
        const int first = 16 + static_cast<int>((offset - base) * 8);
        for (int i = 0; i < 32 && first + i < kExceptions; ++i)
            value |= (set[first + i] ? 1u : 0u) << i;
        return value;
    };
    if (offset >= 0x100 && offset < 0x120) return bits(enabled_, 0x100);
    if (offset >= 0x180 && offset < 0x1A0) return bits(enabled_, 0x180);
    if (offset >= 0x200 && offset < 0x220) return bits(pending_, 0x200);
    if (offset >= 0x280 && offset < 0x2A0) return bits(pending_, 0x280);
    if (offset >= 0x300 && offset < 0x320) return bits(active_, 0x300);
    if (offset >= 0x400 && offset < 0x400 + kExceptions - 16) {
        std::uint32_t value = 0;
        for (std::uint32_t i = 0; i < 4 && 16 + offset - 0x400 + i < kExceptions; ++i)
            value |= static_cast<std::uint32_t>(priority_[16 + offset - 0x400 + i]) << (8 * i);
        return value;
    }
    if (offset >= 0xD18 && offset < 0xD24) {
        std::uint32_t value = 0;
        for (std::uint32_t i = 0; i < 4; ++i)
            value |= static_cast<std::uint32_t>(priority_[4 + offset - 0xD18 + i]) << (8 * i);
        return value;
    }
    switch (offset) {
    case 0x010: {
        const std::uint32_t value = systCsr_;
        systCsr_ &= ~0x10000u;  // COUNTFLAG clears on read
        // Read repeatedly while counting and not yet expired: a delay loop
        // waiting for COUNTFLAG, so let the time until the expiry pass.
        if ((value & 1) && !(value & 0x10000)) {
            systPolls_ = now_ - systPolledAt_ < 64 ? systPolls_ + 1 : 0;
            systPolledAt_ = now_;
            static const bool noSystIdle = std::getenv("HYDRA_NO_SYST_IDLE") != nullptr;  // experiment
            if (systPolls_ >= 2 && !noSystIdle)
                idleUntilEvent();
        }
        return value;
    }
    case 0x014: return systReload_;
    case 0x018: return systCurrent_;
    case 0x01C: return 0x80000000 | 9000;  // NOREF, 1 ms at 9 MHz
    case 0xD00: return 0x411FC231;         // Cortex-M3 r1p1
    case 0xD04: {
        int vectPending = 0;
        for (int e = 2; e < kExceptions && !vectPending; ++e)
            if (pending_[e] && (e < 16 || enabled_[e]))
                vectPending = e;
        bool irqPending = false;
        for (int e = 16; e < kExceptions; ++e)
            irqPending |= pending_[e];
        return (ipsr_ & 0x1FF) | static_cast<std::uint32_t>(vectPending) << 12 | (irqPending ? 1u << 22 : 0) |
               (pending_[kSysTick] ? 1u << 26 : 0) | (pending_[kPendSv] ? 1u << 28 : 0);
    }
    case 0xD08: return vtor_;
    case 0xD0C: return 0xFA050000 | priGroup_ << 8;
    case 0xD10: return scr_;
    case 0xD14: return ccr_;
    default: return 0;
    }
}

void CortexM3::scsWrite(std::uint32_t offset, std::uint32_t value) {
    const auto setBits = [&](std::array<bool, kExceptions>& set, std::uint32_t base, bool state) {
        const int first = 16 + static_cast<int>((offset - base) * 8);
        for (int i = 0; i < 32 && first + i < kExceptions; ++i)
            if (value >> i & 1)
                set[first + i] = state;
    };
    if (offset >= 0x100 && offset < 0x120) return setBits(enabled_, 0x100, true);
    if (offset >= 0x180 && offset < 0x1A0) return setBits(enabled_, 0x180, false);
    if (offset >= 0x200 && offset < 0x220) return setBits(pending_, 0x200, true);
    if (offset >= 0x280 && offset < 0x2A0) return setBits(pending_, 0x280, false);
    if (offset >= 0x400 && offset < 0x400 + kExceptions - 16) {
        for (std::uint32_t i = 0; i < 4 && 16 + offset - 0x400 + i < kExceptions; ++i)
            priority_[16 + offset - 0x400 + i] = static_cast<std::uint8_t>(value >> (8 * i) & 0xF0);  // 4 bits implemented
        return;
    }
    if (offset >= 0xD18 && offset < 0xD24) {
        for (std::uint32_t i = 0; i < 4; ++i)
            priority_[4 + offset - 0xD18 + i] = static_cast<std::uint8_t>(value >> (8 * i) & 0xF0);
        return;
    }
    switch (offset) {
    case 0x010: systCsr_ = (systCsr_ & 0x10000) | (value & 7); break;
    case 0x014: systReload_ = value & 0xFFFFFF; break;
    case 0x018:
        systCurrent_ = 0;
        systCsr_ &= ~0x10000u;
        break;
    case 0xD04:
        if (value & 1u << 28) pending_[kPendSv] = true;
        if (value & 1u << 27) pending_[kPendSv] = false;
        if (value & 1u << 26) pending_[kSysTick] = true;
        if (value & 1u << 25) pending_[kSysTick] = false;
        break;
    case 0xD08: vtor_ = value & 0x3FFFFF80; break;
    case 0xD0C:
        if ((value >> 16) == 0x05FA) {
            priGroup_ = value >> 8 & 7;
            if (value & 4)
                fail("system reset requested (AIRCR.SYSRESETREQ)");
        }
        break;
    case 0xD10: scr_ = value; break;
    case 0xD14: ccr_ = value; break;
    case 0xF00:
        if (16 + (value & 0x1FF) < static_cast<std::uint32_t>(kExceptions))
            pending_[16 + (value & 0x1FF)] = true;
        break;
    default: break;
    }
}

void CortexM3::tickSysTick(std::uint64_t from, std::uint64_t to) {
    if (!(systCsr_ & 1) || to <= from)
        return;
    std::uint64_t ticks = (systCsr_ & 4) ? to - from : (to >> 3) - (from >> 3);  // core clock or core/8
    while (ticks > 0) {
        if (systCurrent_ == 0) {
            systCurrent_ = systReload_;
            --ticks;
            if (systReload_ == 0)
                return;
            continue;
        }
        if (ticks < systCurrent_) {
            systCurrent_ -= static_cast<std::uint32_t>(ticks);
            return;
        }
        ticks -= systCurrent_;
        systCurrent_ = 0;
        systCsr_ |= 0x10000;
        if (systCsr_ & 2)
            pending_[kSysTick] = true;
    }
}

// --- run loop -------------------------------------------------------------------

void CortexM3::stopAfterAccess() {
    if (!inRun_)
        return;
    stopRequested_ = true;
    jit_->HaltExecution(Dynarmic::HaltReason::MemoryAbort);  // checked after each access
}

bool CortexM3::PreCodeReadHook(bool is_thumb, std::uint32_t pc, Dynarmic::A32::IREmitter& ir) {
    const Dynarmic::A32::LocationDescriptor location{ir.block.Location()};
    const std::uint32_t start = location.PC();
    if (start == pc) {
        ++blocksTranslated;
        static const bool trace = std::getenv("HYDRA_TRACE_PANEL_TRANSLATE") != nullptr;  // diagnostic
        if (trace)
            std::fprintf(stderr, "panel translate %08x at %.4f s\n", pc, static_cast<double>(now_) / 72e6);
    }
    if (!profileBlocks_ || !is_thumb)
        return true;
    if (start < kFlashBase || start >= kFlashBase + kFlashSize)
        return true;
    const std::uint32_t index = (start - kFlashBase) / 2;
    if (pc == start) {
        blockLengths_[index] = 0;
        if (!location.IT().IsInITBlock())
            ir.CallHostFunction(&CortexM3::blockThunk, ir.Imm64(reinterpret_cast<std::uint64_t>(this)), ir.Imm64(index));
    }
    ++blockLengths_[index];  // one call per instruction
    return true;
}

void CortexM3::blockThunk(std::uint64_t core, std::uint64_t index) {
    ++reinterpret_cast<CortexM3*>(core)->blockEntries_[index];
}

void CortexM3::setProfileBlocks(bool enabled) {
    if (enabled == profileBlocks_)
        return;
    profileBlocks_ = enabled;
    if (enabled && blockEntries_.empty()) {
        blockEntries_.assign(kFlashSize / 2, 0);
        blockLengths_.assign(kFlashSize / 2, 0);
    }
    jit_->ClearCache();
}

std::map<std::uint32_t, std::pair<std::uint64_t, std::uint32_t>> CortexM3::blockCounts() const {
    std::map<std::uint32_t, std::pair<std::uint64_t, std::uint32_t>> counts;
    for (std::uint32_t i = 0; i < blockEntries_.size(); ++i)
        if (blockEntries_[i])
            counts[kFlashBase + 2 * i] = {blockEntries_[i], blockLengths_[i]};
    return counts;
}

void CortexM3::idleUntilEvent() {
    if (idleSites)
        ++(*idleSites)[static_cast<std::uint64_t>(regs()[14]) << 32 | regs()[15]];
    if (!inRun_)
        return;
    pollIdle_ = true;
    jit_->HaltExecution(Dynarmic::HaltReason::MemoryAbort);
}

namespace {
// Cycles per run slice at most; timer, DMA and SysTick events cut slices
// short as needed, so this only bounds how late other peripheral state
// (e.g. GPIO inputs set by the board) is noticed.
constexpr std::uint64_t kMaxSlice = 4096;
}  // namespace

void CortexM3::run(std::uint64_t ticks) {
    const std::uint64_t end = now_ + ticks;
    std::uint64_t lastTick = now_;
    while (now_ < end && fatal_.empty()) {
        const bool eventsDue = eventsChanged || now_ >= nextEventCache_;
        if (eventsDue && advance)
            advance(now_);
        tickSysTick(lastTick, now_);
        lastTick = now_;
        if (const int e = pendingException()) {
            sleeping_ = false;
            pollIdle_ = false;
            enterException(e, regs()[15]);
        }

        // While polling idle, nothing happens before the next event.
        static const std::uint64_t maxSlice = std::getenv("HYDRA_PANEL_SLICE") ? std::strtoull(std::getenv("HYDRA_PANEL_SLICE"), nullptr, 10) : kMaxSlice;  // experiment
        std::uint64_t slice = pollIdle_ ? end - now_ : std::min<std::uint64_t>(end - now_, maxSlice);
        if (nextEvent) {
            if (eventsDue) {
                eventsChanged = false;
                nextEventCache_ = nextEvent(now_);
            }
            if (nextEventCache_ > now_)
                slice = std::min(slice, nextEventCache_ - now_);
        }
        // SysTick expiry: a slice boundary when it interrupts, or while the CPU
        // idles in a poll for it (a cleared counter reloads on the next tick).
        if ((systCsr_ & 1) && ((systCsr_ & 2) || pollIdle_) && (systCurrent_ != 0 || systReload_ != 0)) {
            const std::uint64_t remaining = systCurrent_ != 0 ? systCurrent_ : systReload_ + 1ull;
            const std::uint64_t untilTick = (systCsr_ & 4) ? remaining : 8ull * remaining;
            slice = std::min(slice, std::max<std::uint64_t>(untilTick, 1));
        }
        slice = std::max<std::uint64_t>(slice, 1);

        if (sleeping_ || pollIdle_) {
            pollIdle_ = false;  // the poll runs again after this
            now_ += slice;
            idleTicks_ += slice;
            continue;
        }
        ticksRemaining_ = slice;
        stop_ = Stop::None;
        const std::uint64_t before = now_;
        inRun_ = true;
        jit_->Run();
        inRun_ = false;
        if (now_ == before)
            now_ += 1;  // halted before retiring anything: keep time moving
        switch (stop_) {
        case Stop::SystemInstruction:
            if (stopPc_ == ~0u)
                enterException(kSvCall, regs()[15]);  // SVC is taken at once
            else
                executeSystemInstruction(stopPc_);
            break;
        case Stop::ExceptionReturn: exceptionReturn(stopPc_); break;
        default: break;
        }
        if (std::exchange(stopRequested_, false))
            break;
    }
}

}  // namespace hydra
