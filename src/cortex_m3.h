#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include <dynarmic/interface/A32/a32.h>
#include <dynarmic/interface/A32/config.h>

#include "bus.h"

namespace hydra {

// A Cortex-M3 (ARMv7-M) on Dynarmic in Thumb mode, with the M-profile system
// model Dynarmic lacks: exception entry/return with stacking, MSP/PSP and
// CONTROL, PRIMASK/FAULTMASK/BASEPRI, the NVIC, SCB and SysTick.
//
// Memory: 512 KiB flash at 0x08000000 (aliased at 0), 64 KiB SRAM at
// 0x20000000, bit-band aliases at 0x22000000 (SRAM) and 0x42000000
// (peripherals), the system control space at 0xE000E000; everything else
// below 0xE0000000 goes to the peripheral bus.
class CortexM3 final : public Dynarmic::A32::UserCallbacks {
public:
    static constexpr std::uint32_t kFlashBase = 0x08000000, kFlashSize = 512u << 10;
    static constexpr std::uint32_t kSramBase = 0x20000000, kSramSize = 64u << 10;
    static constexpr int kExceptions = 16 + 68;
    static constexpr int kSvCall = 11, kPendSv = 14, kSysTick = 15;

    explicit CortexM3(MmioBus& bus);
    ~CortexM3() override;

    void loadFlash(std::uint32_t offset, const std::vector<std::uint8_t>& data);
    // Reset from the vector table at `vectors` (initial SP, reset handler).
    void reset(std::uint32_t vectors);
    // Runs for `ticks` processor cycles (one instruction per cycle).
    void run(std::uint64_t ticks);
    // From a read callback during run(): ends run() with the PC at the reading
    // instruction, which runs again when execution resumes, so the read must be
    // repeatable without side effects. No effect outside the JIT (e.g. for DMA).
    void stopAfterAccess();
    // From a read callback during run() that found the CPU polling for a
    // peripheral event: the time up to the next event or interrupt passes at
    // once, then the read runs again (same conditions as stopAfterAccess).
    void idleUntilEvent();
    void setIrqPending(int irq) { pending_[16 + irq] = true; }

    std::uint64_t now() const { return now_; }
    std::uint64_t idleTicks() const { return idleTicks_; }  // cycles passed while sleeping or idle-polling
    std::uint64_t exceptionsTaken() const { return exceptionsTaken_; }
    std::uint64_t blocksTranslated = 0;
    bool sleeping() const { return sleeping_; }
    const std::string& fatalMessage() const { return fatal_; }
    std::array<std::uint32_t, 16>& regs() { return jit_->Regs(); }
    std::uint32_t ipsr() const { return ipsr_; }
    // Diagnostic: idle polls by (LR << 32 | PC), if set.
    std::map<std::uint64_t, std::uint64_t>* idleSites = nullptr;
    // Whether an exception's handler is running (possibly preempted).
    bool handlerActive(int exception) const { return active_[static_cast<std::size_t>(exception)]; }
    bool interruptsMasked() const { return primask_ != 0 || faultmask_ != 0; }

    // Board hooks: the earliest cycle at which a peripheral needs attention,
    // and a call to bring peripherals up to the current cycle.
    std::function<std::uint64_t(std::uint64_t now)> nextEvent;
    std::function<void(std::uint64_t now)> advance;
    // Set when a peripheral's next event may have changed; until then, and
    // before the cached next event, advance and nextEvent are not called.
    bool eventsChanged = true;

    std::uint32_t read(std::uint32_t addr, int size);
    void write(std::uint32_t addr, std::uint32_t value, int size);

    // Dynarmic::A32::UserCallbacks
    std::optional<std::uint32_t> MemoryReadCode(std::uint32_t vaddr) override;
    bool PreCodeReadHook(bool is_thumb, std::uint32_t pc, Dynarmic::A32::IREmitter& ir) override;

    // Diagnostic: exact instruction profile of the flash code (see
    // CpuCore::setProfileBlocks). Blocks that start inside an IT block are not
    // counted. blockCounts(): start pc -> (entries, instructions per entry).
    void setProfileBlocks(bool enabled);
    bool profileBlocks() const { return profileBlocks_; }
    std::map<std::uint32_t, std::pair<std::uint64_t, std::uint32_t>> blockCounts() const;
    bool IsSystemInstruction(bool is_thumb, std::uint32_t instruction) override;
    std::uint8_t MemoryRead8(std::uint32_t a) override { return static_cast<std::uint8_t>(read(a, 1)); }
    std::uint16_t MemoryRead16(std::uint32_t a) override { return static_cast<std::uint16_t>(read(a, 2)); }
    std::uint32_t MemoryRead32(std::uint32_t a) override { return read(a, 4); }
    std::uint64_t MemoryRead64(std::uint32_t a) override { return read(a, 4) | std::uint64_t(read(a + 4, 4)) << 32; }
    void MemoryWrite8(std::uint32_t a, std::uint8_t v) override { write(a, v, 1); }
    void MemoryWrite16(std::uint32_t a, std::uint16_t v) override { write(a, v, 2); }
    void MemoryWrite32(std::uint32_t a, std::uint32_t v) override { write(a, v, 4); }
    void MemoryWrite64(std::uint32_t a, std::uint64_t v) override {
        write(a, static_cast<std::uint32_t>(v), 4);
        write(a + 4, static_cast<std::uint32_t>(v >> 32), 4);
    }
    bool MemoryWriteExclusive8(std::uint32_t a, std::uint8_t v, std::uint8_t e) override { return writeExclusive(a, v, e, 1); }
    bool MemoryWriteExclusive16(std::uint32_t a, std::uint16_t v, std::uint16_t e) override { return writeExclusive(a, v, e, 2); }
    bool MemoryWriteExclusive32(std::uint32_t a, std::uint32_t v, std::uint32_t e) override { return writeExclusive(a, v, e, 4); }
    void InterpreterFallback(std::uint32_t pc, std::size_t num_instructions) override;
    void CallSVC(std::uint32_t swi) override;
    void ExceptionRaised(std::uint32_t pc, Dynarmic::A32::Exception exception) override;
    void AddTicks(std::uint64_t ticks) override;
    std::uint64_t GetTicksRemaining() override { return ticksRemaining_; }

private:
    enum class Stop { None, SystemInstruction, ExceptionReturn, Fatal };

    bool writeExclusive(std::uint32_t a, std::uint32_t value, std::uint32_t expected, int size);
    std::uint32_t scsRead(std::uint32_t offset);
    void scsWrite(std::uint32_t offset, std::uint32_t value);

    void executeSystemInstruction(std::uint32_t pc);
    int priority(int exception) const;
    int executionPriority() const;
    int pendingException() const;  // 0 if none can preempt
    void enterException(int exception, std::uint32_t returnAddress);
    void exceptionReturn(std::uint32_t excReturn);
    void setActiveStack(bool process);
    std::uint32_t xpsr();
    void tickSysTick(std::uint64_t from, std::uint64_t to);
    void fail(std::string message);

    MmioBus& bus_;
    std::uint64_t nextEventCache_ = 0;
    std::vector<std::uint8_t> flash_, sram_;
    std::unique_ptr<std::array<std::uint8_t*, Dynarmic::A32::UserConfig::NUM_PAGE_TABLE_ENTRIES>> pageTable_;
    std::unique_ptr<Dynarmic::A32::Jit> jit_;

    std::uint64_t now_ = 0, ticksRemaining_ = 0;
    Stop stop_ = Stop::None;
    bool inRun_ = false, stopRequested_ = false, pollIdle_ = false;
    std::uint64_t idleTicks_ = 0, exceptionsTaken_ = 0;
    static void blockThunk(std::uint64_t core, std::uint64_t index);
    bool profileBlocks_ = false;
    std::vector<std::uint64_t> blockEntries_;   // by (pc - flash base) / 2
    std::vector<std::uint32_t> blockLengths_;
    std::uint64_t systPolledAt_ = 0;
    int systPolls_ = 0;
    std::uint32_t stopPc_ = 0;
    bool sleeping_ = false;
    std::string fatal_;

    // Special registers.
    std::uint32_t msp_ = 0, psp_ = 0;  // the inactive stack pointer lives here
    bool usingPsp_ = false;
    std::uint32_t control_ = 0, primask_ = 0, faultmask_ = 0, basepri_ = 0;
    std::uint32_t ipsr_ = 0;

    // NVIC and SCB.
    std::array<bool, kExceptions> enabled_{}, pending_{}, active_{};
    std::array<std::uint8_t, kExceptions> priority_{};
    std::uint32_t vtor_ = 0, priGroup_ = 0, scr_ = 0, ccr_ = 0;

    // SysTick.
    std::uint32_t systCsr_ = 0, systReload_ = 0, systCurrent_ = 0;
};

}  // namespace hydra
