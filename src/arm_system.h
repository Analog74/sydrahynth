#pragma once

#include <array>
#include <cstdint>

// ARMv7-A privileged state that Dynarmic (a user-mode JIT) does not model:
// processor modes with banked registers, SPSRs, exception entry and the
// system instructions that move between modes. Dynarmic holds the registers of
// the current mode; this class holds every other bank.
namespace hydra::arm {

enum Mode : std::uint32_t {
    USR = 0x10,
    FIQ = 0x11,
    IRQ = 0x12,
    SVC = 0x13,
    MON = 0x16,
    ABT = 0x17,
    UND = 0x1B,
    SYS = 0x1F,
};

namespace psr {
constexpr std::uint32_t M = 0x1F, T = 1u << 5, F = 1u << 6, I = 1u << 7, A = 1u << 8, E = 1u << 9;
constexpr std::uint32_t J = 1u << 24, IT = 0x0600FC00, C = 1u << 29;
}  // namespace psr

enum class ExceptionKind { Reset, Undefined, SupervisorCall, PrefetchAbort, DataAbort, Irq, Fiq };

// What the system layer needs from the CPU core.
class CpuAccess {
public:
    virtual ~CpuAccess() = default;
    virtual std::array<std::uint32_t, 16>& regs() = 0;
    virtual std::uint32_t cpsr() = 0;
    virtual void setCpsr(std::uint32_t value) = 0;
    virtual std::uint32_t read32(std::uint32_t addr) = 0;
    virtual void write32(std::uint32_t addr, std::uint32_t value) = 0;
    virtual std::uint32_t vectorBase() = 0;
    virtual bool exceptionsInThumb() = 0;  // SCTLR.TE
    virtual bool exceptionsBigEndian() = 0;  // SCTLR.EE
};

class SystemState {
public:
    enum class Result { Done, Unsupported };

    explicit SystemState(CpuAccess& cpu) : cpu_(cpu) {}

    // True for instructions this class must emulate instead of Dynarmic.
    static bool isSystemInstruction(std::uint32_t instruction);

    // Emulates a claimed instruction at pc. Dynarmic has already advanced PC
    // past it, so instructions that do not branch leave PC alone.
    Result execute(std::uint32_t instruction, std::uint32_t pc);

    void enterException(ExceptionKind kind, std::uint32_t returnAddress);
    void writeCpsr(std::uint32_t value);

    std::uint32_t mode() { return cpu_.cpsr() & psr::M; }
    std::uint32_t spsr();
    void setSpsr(std::uint32_t value);

private:
    struct Bank {
        std::uint32_t r13 = 0, r14 = 0, spsr = 0;
    };

    static int bankIndex(std::uint32_t mode);
    void switchBanks(std::uint32_t from, std::uint32_t to);
    std::uint32_t userReg(int n);
    void setUserReg(int n, std::uint32_t value);
    std::uint32_t bankedSp(std::uint32_t mode);
    void setBankedSp(std::uint32_t mode, std::uint32_t value);
    void exceptionReturn(std::uint32_t newPc, std::uint32_t newCpsr);

    Result msr(std::uint32_t instruction, std::uint32_t value);
    Result dataProcessingReturn(std::uint32_t instruction, std::uint32_t pc);
    Result loadStoreMultipleUser(std::uint32_t instruction, std::uint32_t pc);
    Result vfpSystemRegister(std::uint32_t instruction);

    CpuAccess& cpu_;
    std::array<Bank, 7> banks_{};  // indexed by bankIndex()
    std::array<std::uint32_t, 5> usrHigh_{};  // r8-r12 while in FIQ mode
    std::array<std::uint32_t, 5> fiqHigh_{};  // r8_fiq-r12_fiq while not in FIQ mode
    std::uint32_t fpexc_ = 0;
};

}  // namespace hydra::arm
