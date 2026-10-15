#pragma once

#include <cstdint>
#include <deque>
#include <functional>
#include <map>

#include <dynarmic/interface/A32/coprocessor.h>

namespace hydra {

constexpr std::uint32_t cp15Key(unsigned opc1, unsigned crn, unsigned crm, unsigned opc2) {
    return opc1 << 12 | crn << 8 | crm << 4 | opc2;
}

// CP15 system control coprocessor of a single Cortex-A5 core. Registers are
// keyed as (opc1, CRn, CRm, opc2). There is no MMU translation: the firmware
// runs identity mapped, so SCTLR/TTBR changes are only reported.
class Cp15 : public Dynarmic::A32::Coprocessor {
public:
    struct Hooks {
        std::function<void(std::uint32_t sctlr, std::uint32_t ttbr0)> mmuChanged;
        std::function<void()> instructionCacheInvalidated;
    };

    static constexpr std::uint32_t key(unsigned opc1, unsigned crn, unsigned crm, unsigned opc2) {
        return cp15Key(opc1, crn, crm, opc2);
    }

    explicit Cp15(Hooks hooks);

    std::uint32_t sctlr() const { return regs_.at(kSctlr); }
    std::uint32_t vbar() const { return regs_.at(kVbar); }

    // Accesses to registers this model has no meaning for, keyed as above.
    const std::map<std::uint32_t, std::uint64_t>& unknownAccesses() const { return unknown_; }

    std::optional<Callback> CompileInternalOperation(bool two, unsigned opc1, Dynarmic::A32::CoprocReg CRd,
                                                     Dynarmic::A32::CoprocReg CRn, Dynarmic::A32::CoprocReg CRm,
                                                     unsigned opc2) override;
    CallbackOrAccessOneWord CompileSendOneWord(bool two, unsigned opc1, Dynarmic::A32::CoprocReg CRn,
                                               Dynarmic::A32::CoprocReg CRm, unsigned opc2) override;
    CallbackOrAccessTwoWords CompileSendTwoWords(bool two, unsigned opc, Dynarmic::A32::CoprocReg CRm) override;
    CallbackOrAccessOneWord CompileGetOneWord(bool two, unsigned opc1, Dynarmic::A32::CoprocReg CRn,
                                              Dynarmic::A32::CoprocReg CRm, unsigned opc2) override;
    CallbackOrAccessTwoWords CompileGetTwoWords(bool two, unsigned opc, Dynarmic::A32::CoprocReg CRm) override;
    std::optional<Callback> CompileLoadWords(bool two, bool long_transfer, Dynarmic::A32::CoprocReg CRd,
                                             std::optional<std::uint8_t> option) override;
    std::optional<Callback> CompileStoreWords(bool two, bool long_transfer, Dynarmic::A32::CoprocReg CRd,
                                              std::optional<std::uint8_t> option) override;

private:
    static constexpr std::uint32_t kSctlr = cp15Key(0, 1, 0, 0);
    static constexpr std::uint32_t kTtbr0 = cp15Key(0, 2, 0, 0);
    static constexpr std::uint32_t kVbar = cp15Key(0, 12, 0, 0);

    struct Slot {
        Cp15* self;
        std::uint32_t key;
    };
    Callback callbackFor(std::uint32_t key, bool write);
    static std::uint64_t readThunk(void* slot, std::uint32_t, std::uint32_t);
    static std::uint64_t writeThunk(void* slot, std::uint32_t value, std::uint32_t);

    std::uint32_t read(std::uint32_t key);
    void write(std::uint32_t key, std::uint32_t value);

    Hooks hooks_;
    std::map<std::uint32_t, std::uint32_t> regs_;
    std::map<std::uint32_t, std::uint64_t> unknown_;
    std::deque<Slot> slots_;  // stable addresses handed to compiled code
};

}  // namespace hydra
