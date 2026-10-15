#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "bus.h"

namespace hydra {

// Tone generator at 0xF6000000: 512 sample-playback voices that read 16-bit
// samples straight from NOR flash (where the wave ROM lives) and mix into 24
// stereo buses, which form the main DSP block's input each sample.
//
// Registers:
//   +0x00  register address (bit 31 selects voices 256-511)
//   +0x04  write: store data at the address; read: data latched by a read command
//   +0x08  write 5: latch the register at the address into +0x04; reads 0x10100 (status)
//   +0x0C, +0x14, +0x20  plain registers
//
// Voice registers (v = voice within the half selected by bit 31):
//   0x0000 + 4v + 2           control: bits 0-5 right level, 8-13 left level
//                             (0-63), bits 24-28 bus pair (bus = (bits >> 23) & 0x3E)
//   0x0C00 + 4v + 1           gain: bits 0-15 target (0x4982 = unity), bits 16-31
//                             ramp step per sample (0: jump immediately)
//   0x1000 + 8v + 1           start: bits 4-31 sample index, bits 0-3 flags
//                             (bit 1: voice plays); restarts the voice. Reads
//                             back the live position (sample index << 4), flag
//                             bits 1-3, and bit 0 set while the voice plays
//   0x1000 + 8v + 2 / + 3     loop start / loop end sample index (bits 4-31)
//   0x1000 + 8v + 4           pitch: added to the position (32.32 fixed point,
//                             fraction in the upper 16 bits) << 16 per sample
// Every register write is also kept for read-back.
class ToneGenerator : public Device {
public:
    static constexpr int kVoices = 512;
    static constexpr int kBuses = 48;  // 24 stereo pairs
    static constexpr std::uint32_t kUnityGain = 0x4982;

    // `flash` is the NOR flash contents the voices read samples from.
    explicit ToneGenerator(const std::vector<std::uint8_t>& flash) : flash_(flash) {}

    std::string_view name() const override { return "tg"; }
    std::uint32_t read(std::uint32_t offset, int size) override;
    void write(std::uint32_t offset, std::uint32_t value, int size) override;

    // Renders one sample into out[0..kBuses).
    void render(std::uint32_t* out);

    std::uint64_t dataWrites() const { return dataWrites_; }
    // Diagnostic: one line per playing voice.
    std::string describeActiveVoices() const;
    int activeVoices() const { return static_cast<int>(active_.size()); }

private:
    struct Voice {
        std::uint64_t position = 0;  // 32.32 sample index
        std::uint32_t loopStart = 0, loopEnd = 0;
        std::uint32_t pitch = 0;
        std::uint32_t flags = 0;
        std::uint32_t control = 0;
        std::int32_t gain = kUnityGain, targetGain = kUnityGain, gainStep = 0;
    };

    void writeVoice(std::uint32_t address, std::uint32_t value);
    std::int64_t sampleAt(std::uint64_t byteAddress) const;

    const std::vector<std::uint8_t>& flash_;
    std::array<Voice, kVoices> voices_{};
    std::vector<std::uint16_t> active_;
    bool activeDirty_ = true;
    std::unordered_map<std::uint32_t, std::uint32_t> regs_;
    std::uint64_t dataWrites_ = 0;
    std::uint32_t address_ = 0, readData_ = 0, reg0c_ = 0, reg14_ = 0, reg20_ = 0;
};

}  // namespace hydra
