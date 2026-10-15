#include "tone_generator.h"

#include <algorithm>
#include <cstring>
#include <format>

namespace hydra {

std::uint32_t ToneGenerator::read(std::uint32_t offset, int) {
    switch (offset) {
    case 0x00: return address_;
    case 0x04: return readData_;
    case 0x08: return 0x10100;
    case 0x0C: return reg0c_;
    case 0x14: return reg14_;
    case 0x20: return reg20_;
    default: return 0;
    }
}

void ToneGenerator::write(std::uint32_t offset, std::uint32_t value, int) {
    switch (offset) {
    case 0x00: address_ = value; break;
    case 0x04:
        regs_[address_] = value;
        ++dataWrites_;
        writeVoice(address_, value);
        break;
    case 0x08:
        if (value == 5) {
            const auto it = regs_.find(address_);
            readData_ = it == regs_.end() ? 0 : it->second;
            // A voice's start register reads back its live position and state.
            const std::uint32_t a = address_ & 0x7FFFFFFF;
            if ((a & 0x7FFFF800) == 0x1000 && (a & 7) == 1) {
                const Voice& v = voices_[(address_ >> 31 << 8) | (a - 0x1000) >> 3];
                readData_ = (static_cast<std::uint32_t>(v.position >> 28) & 0xFFFFFFF0) | (v.flags & 0xE) |
                            ((v.flags & 2) ? 1 : 0);
            }
        }
        break;
    case 0x0C: reg0c_ = value; break;
    case 0x14: reg14_ = value; break;
    case 0x20: reg20_ = value; break;
    default: break;
    }
}

void ToneGenerator::writeVoice(std::uint32_t address, std::uint32_t value) {
    const std::uint32_t half = address >> 31 << 8;
    const std::uint32_t a = address & 0x7FFFFFFF;
    if ((a & 0x7FFFF800) == 0x1000) {
        Voice& v = voices_[half | (a - 0x1000) >> 3];
        switch (a & 7) {
        case 1:
            v.position = static_cast<std::uint64_t>(value >> 4) << 32;
            v.flags = value & 0xF;
            activeDirty_ = true;
            break;
        case 2: v.loopStart = value >> 4; break;
        case 3: v.loopEnd = value >> 4; break;
        case 4:
            v.pitch = value;
            activeDirty_ = true;
            break;
        }
    } else if (a <= 0x3FF) {
        if ((a & 3) == 2)
            voices_[half | a >> 2].control = value;
    } else if ((a & 0x7FFFFC00) == 0xC00) {
        if ((a & 3) == 1) {
            Voice& v = voices_[half | (a - 0xC00) >> 2];
            v.targetGain = static_cast<std::int32_t>(value & 0xFFFF);
            v.gainStep = static_cast<std::int32_t>(value >> 16);
            if (value <= 0xFFFF)
                v.gain = v.targetGain;
        }
    }
}

std::int64_t ToneGenerator::sampleAt(std::uint64_t byteAddress) const {
    if (byteAddress > 0x7FFFFFE || byteAddress + 2 > flash_.size())
        return -1;
    std::int16_t s;
    std::memcpy(&s, flash_.data() + byteAddress, 2);
    return s;
}

void ToneGenerator::render(std::uint32_t* out) {
    if (activeDirty_) {
        active_.clear();
        for (int i = 0; i < kVoices; ++i)
            if ((voices_[i].flags & 2) && voices_[i].pitch != 0)
                active_.push_back(static_cast<std::uint16_t>(i));
        activeDirty_ = false;
    }

    std::array<std::int64_t, kBuses> acc{};
    for (const std::uint16_t index : active_) {
        Voice& v = voices_[index];
        const std::uint32_t left = v.control >> 8 & 0x3F, right = v.control & 0x3F;
        const std::uint32_t bus = v.control >> 23 & 0x3E;
        if ((left | right) == 0 || bus > 0x2F)
            continue;

        if (v.loopEnd > v.loopStart) {
            const std::uint64_t end = static_cast<std::uint64_t>(v.loopEnd) << 32;
            if (v.position >= end) {
                const std::uint64_t length = static_cast<std::uint64_t>(v.loopEnd - v.loopStart) << 32;
                v.position = (v.position - end) % length + end - length;
            }
        }
        if (v.gain != v.targetGain) {
            const std::int32_t diff = v.targetGain - v.gain;
            if ((diff > 0 ? diff : -diff) > v.gainStep)
                v.gain += diff > 0 ? v.gainStep : -v.gainStep;
            else
                v.gain = v.targetGain;
        }

        // Linear interpolation between neighbouring samples; the fraction is
        // the upper 16 bits of the position's fractional part.
        const std::uint64_t byteAddress = (v.position >> 31) & 0xFFFFFFFE;
        const std::int64_t s0 = sampleAt(byteAddress), s1 = sampleAt(byteAddress + 2);
        const std::int64_t fraction = (v.position >> 16) & 0xFFFF;
        std::int64_t value = ((s0 << 16) + fraction * (s1 - s0)) >> 8;
        if (v.gain != static_cast<std::int32_t>(kUnityGain))
            value = v.gain * value / static_cast<std::int64_t>(kUnityGain);
        acc[bus] += value * left / 63;
        acc[bus + 1] += value * right / 63;
        v.position += static_cast<std::uint64_t>(v.pitch) << 16;
    }
    for (int i = 0; i < kBuses; ++i)
        out[i] = static_cast<std::uint32_t>(std::clamp<std::int64_t>(acc[i], INT32_MIN, INT32_MAX));
}

std::string ToneGenerator::describeActiveVoices() const {
    std::string out;
    for (const std::uint16_t i : active_) {
        const Voice& v = voices_[i];
        out += std::format("voice {:3d}: pos {:08x}.{:08x} loop {:08x}-{:08x} pitch {:08x} flags {:x} ctrl {:08x} gain {:04x}->{:04x}\n",
                           i, static_cast<std::uint32_t>(v.position >> 32), static_cast<std::uint32_t>(v.position), v.loopStart,
                           v.loopEnd, v.pitch, v.flags, v.control, v.gain, v.targetGain);
    }
    return out;
}

}  // namespace hydra
