#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <algorithm>
#include <map>
#include <utility>
#include <vector>
#include <memory>
#include <string>

#include <dynarmic/interface/A32/a32.h>
#include <dynarmic/interface/A32/config.h>
#include <dynarmic/interface/exclusive_monitor.h>

#include "arm_system.h"
#include "bus.h"
#include "cp15.h"

namespace hydra {

// The main ARM core: Dynarmic for user-level code, arm::SystemState and Cp15
// for everything privileged.
class CpuCore final : public Dynarmic::A32::UserCallbacks, public arm::CpuAccess {
public:
    enum class Stop { SliceDone, WaitForInterrupt, Fatal };

    CpuCore(Memory& memory, MmioBus& bus);
    ~CpuCore() override;

    void reset(std::uint32_t pc, std::uint32_t cpsr);
    Stop run(std::uint64_t ticks);
    // Profiling (diagnostic): from any thread, asks the running code to stop at
    // the next block boundary; run() then counts the (PC, LR) there and goes on.
    void requestSample();
    // Diagnostic: why Run() returned (Dynarmic halt reason bits; 0: budget used
    // up), counted while `profileExits` is set.
    bool profileExits = false;
    std::uint64_t svcCount = 0, systemInstructions = 0;  // diagnostic counters (always on, cheap)
    std::uint64_t blocksTranslated = 0;
    // Diagnostic: TSC cycles spent in the C++ side of exceptions while profileExits is set.
    std::uint64_t svcCycles = 0, systemInstructionCycles = 0, irqCycles = 0;
    // Diagnostic: exact instruction profile. While enabled, every block counts
    // its entries (slow; switching it drops the translated code). blockCounts():
    // start pc -> (entries, instructions per entry) for the firmware's code area.
    void setProfileBlocks(bool enabled);
    bool profileBlocks() const { return profileBlocks_; }
    std::map<std::uint32_t, std::pair<std::uint64_t, std::uint32_t>> blockCounts() const;
    void resetBlockCounts() { std::fill(blockEntries_.begin(), blockEntries_.end(), 0); }
    const std::map<std::uint32_t, std::uint64_t>& exitReasons() const { return exitReasons_; }
    // From a read callback: ends the slice with the PC at the reading
    // instruction, which runs again when execution resumes (so the read must be
    // free of side effects). Lets a pending interrupt be taken in the middle of
    // a polling loop instead of at the end of the slice.
    void retryAccess();
    const std::map<std::pair<std::uint32_t, std::uint32_t>, std::uint64_t>& guestSamples() const { return guestSamples_; }
    // Takes an IRQ now if CPSR.I allows it; returns whether it was taken.
    bool takeIrq();

    std::uint64_t ticksExecuted() const { return ticksExecuted_; }
    const std::string& fatalMessage() const { return fatal_; }
    Cp15& cp15() { return *cp15_; }

    // arm::CpuAccess
    std::array<std::uint32_t, 16>& regs() override { return jit_->Regs(); }
    std::uint32_t cpsr() override { return jit_->Cpsr(); }
    void setCpsr(std::uint32_t value) override { jit_->SetCpsr(value); }
    std::uint32_t read32(std::uint32_t addr) override { return read<std::uint32_t>(addr); }
    void write32(std::uint32_t addr, std::uint32_t value) override { write<std::uint32_t>(addr, value); }
    std::uint32_t vectorBase() override;
    bool exceptionsInThumb() override { return cp15_->sctlr() & (1u << 30); }
    bool exceptionsBigEndian() override { return cp15_->sctlr() & (1u << 25); }

    // Dynarmic::A32::UserCallbacks
    std::optional<std::uint32_t> MemoryReadCode(std::uint32_t vaddr) override;
    bool IsSystemInstruction(bool is_thumb, std::uint32_t instruction) override;
    std::uint8_t MemoryRead8(std::uint32_t vaddr) override { return read<std::uint8_t>(vaddr); }
    std::uint16_t MemoryRead16(std::uint32_t vaddr) override { return read<std::uint16_t>(vaddr); }
    std::uint32_t MemoryRead32(std::uint32_t vaddr) override { return read<std::uint32_t>(vaddr); }
    std::uint64_t MemoryRead64(std::uint32_t vaddr) override { return read<std::uint64_t>(vaddr); }
    void MemoryWrite8(std::uint32_t vaddr, std::uint8_t value) override { write(vaddr, value); }
    void MemoryWrite16(std::uint32_t vaddr, std::uint16_t value) override { write(vaddr, value); }
    void MemoryWrite32(std::uint32_t vaddr, std::uint32_t value) override { write(vaddr, value); }
    void MemoryWrite64(std::uint32_t vaddr, std::uint64_t value) override { write(vaddr, value); }
    bool MemoryWriteExclusive8(std::uint32_t vaddr, std::uint8_t value, std::uint8_t expected) override;
    bool MemoryWriteExclusive16(std::uint32_t vaddr, std::uint16_t value, std::uint16_t expected) override;
    bool MemoryWriteExclusive32(std::uint32_t vaddr, std::uint32_t value, std::uint32_t expected) override;
    bool MemoryWriteExclusive64(std::uint32_t vaddr, std::uint64_t value, std::uint64_t expected) override;
    bool PreCodeReadHook(bool is_thumb, std::uint32_t pc, Dynarmic::A32::IREmitter& ir) override;
    void InterpreterFallback(std::uint32_t pc, std::size_t num_instructions) override;
    void CallSVC(std::uint32_t swi) override;
    void ExceptionRaised(std::uint32_t pc, Dynarmic::A32::Exception exception) override;
    void AddTicks(std::uint64_t ticks) override;
    std::uint64_t GetTicksRemaining() override { return ticksRemaining_; }

    // Fast-forwards a count-down delay loop (MOVS Rt, Rc; SUB Rc, Rc, #1; BNE) that
    // starts at the current PC by up to `budget` ticks, with the same result as
    // running it; returns the ticks consumed (0 if there is no such loop).
    std::uint64_t skipDelayLoop(std::uint64_t budget);
    bool delayLoopSkip = true;  // false: stop at delay loops but run them (diagnostic)
    std::uint64_t ticksSkipped() const { return ticksSkipped_; }
    std::uint64_t delayLoopEntries() const { return delayLoopEntries_; }
    // Diagnostic: delay loop entries by (LR << 32 | count), if set.
    std::map<std::uint64_t, std::uint64_t>* delayCallers = nullptr;

    // Diagnostic write watchpoint: onWatchedWrite is called for every write that
    // overlaps [from, to) with (pc of the current block, lr, address, value). RAM
    // pages in the range are taken out of the direct-access page table meanwhile.
    void setWatch(std::uint32_t from, std::uint32_t to);
    std::uint32_t watchFrom() const { return watchFrom_; }
    std::uint32_t watchTo() const { return watchTo_; }
    std::function<void(std::uint32_t pc, std::uint32_t lr, std::uint32_t addr, std::uint64_t value)> onWatchedWrite;

private:
    static constexpr auto kHaltFatal = Dynarmic::HaltReason::UserDefined1;
    static constexpr auto kHaltWfi = Dynarmic::HaltReason::UserDefined2;
    static constexpr auto kHaltSample = Dynarmic::HaltReason::UserDefined4;

    template <typename T> T read(std::uint32_t addr);
    template <typename T> void write(std::uint32_t addr, T value);
    template <typename T> bool writeExclusive(std::uint32_t addr, T value, T expected);
    void fail(std::string message);

    void buildPageTable();
    static void delayLoopThunk(std::uint64_t core);
    static void blockThunk(std::uint64_t core, std::uint64_t pc);

    Memory& memory_;
    // Writable RAM pages for Dynarmic's direct access; everything else (MMIO, flash)
    // goes through the callbacks.
    std::unique_ptr<std::array<std::uint8_t*, Dynarmic::A32::UserConfig::NUM_PAGE_TABLE_ENTRIES>> pageTable_;
    std::uint32_t watchFrom_ = 0, watchTo_ = 0;
    std::map<std::pair<std::uint32_t, std::uint32_t>, std::uint64_t> guestSamples_;
    bool running_ = false;
    std::map<std::uint32_t, std::uint64_t> exitReasons_;
    static constexpr std::uint32_t kProfiledCodeWords = 0x200000 / 4;  // code below 2 MiB
    bool profileBlocks_ = false;
    std::vector<std::uint64_t> blockEntries_;   // by pc / 4
    std::vector<std::uint32_t> blockLengths_;   // by pc / 4
    std::uint64_t ticksSkipped_ = 0, delayLoopEntries_ = 0;
    MmioBus& bus_;
    arm::SystemState system_;
    std::shared_ptr<Cp15> cp15_;
    std::unique_ptr<Dynarmic::ExclusiveMonitor> monitor_;
    std::unique_ptr<Dynarmic::A32::Jit> jit_;

    std::uint64_t ticksRemaining_ = 0;
    std::uint64_t ticksExecuted_ = 0;
    std::string fatal_;
};

}  // namespace hydra
