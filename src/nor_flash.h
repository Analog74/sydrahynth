#pragma once

#include <cstdint>

#include "bus.h"

namespace hydra {

// 128 MiB parallel NOR flash at 0x40000000 with the Spansion/AMD command set
// (it identifies as an S29GL01GS: 128 KiB sectors, 16-bit words). Array reads go
// straight to the memory region; while the chip is in a non-read mode the region
// is taken off the direct path so reads reach this device instead.
// Programming only clears bits and completes instantly, so status polling sees
// the final data at once.
class NorFlash : public Device {
public:
    static constexpr std::uint32_t kSectorSize = 0x20000;

    explicit NorFlash(Memory::Region& array) : array_(array) {}

    std::string_view name() const override { return "nor"; }
    std::uint32_t read(std::uint32_t offset, int size) override;
    void write(std::uint32_t offset, std::uint32_t value, int size) override;

    std::uint64_t wordsProgrammed() const { return wordsProgrammed_; }
    std::uint64_t sectorsErased() const { return sectorsErased_; }
    // Lowest and highest byte offsets changed since construction (empty if first > last).
    std::uint32_t dirtyFirst() const { return dirtyFirst_; }
    std::uint32_t dirtyLast() const { return dirtyLast_; }

private:
    enum class State {
        Read,
        Unlock1,         // got AA@555
        Command,         // got 55@2AA, expecting a command byte
        Program,         // A0: next write programs one word
        BufferCount,     // 25: next write is word count - 1
        BufferData,      // buffered words still to come
        BufferConfirm,   // expecting 29
        EraseUnlock1,    // 80, then AA@555
        EraseUnlock2,    // then 55@2AA
        EraseCommand,    // then 30@sector or 10@555
        Autoselect,      // 90: manufacturer/device ID
        Cfi,             // 98@55: CFI query (not implemented, reads 0)
    };

    void setState(State state);
    void programWord(std::uint32_t offset, std::uint16_t value);
    void erase(std::uint32_t offset, std::uint32_t length);
    void markDirty(std::uint32_t first, std::uint32_t last);

    Memory::Region& array_;
    State state_ = State::Read;
    std::uint32_t bufferWordsLeft_ = 0;
    std::uint64_t wordsProgrammed_ = 0, sectorsErased_ = 0;
    std::uint32_t dirtyFirst_ = ~0u, dirtyLast_ = 0;
};

}  // namespace hydra
