#include "nor_flash.h"

#include <algorithm>
#include <cstring>

namespace hydra {

void NorFlash::setState(State state) {
    state_ = state;
    // Only array reads may bypass the device.
    array_.direct = state_ != State::Autoselect && state_ != State::Cfi;
}

std::uint32_t NorFlash::read(std::uint32_t offset, int size) {
    if (state_ == State::Autoselect) {
        switch ((offset >> 1) & 0xFF) {
        case 0x00: return 0x0001;  // Spansion
        case 0x01: return 0x227E;  // device ID, word 1
        case 0x0E: return 0x2228;  // device ID, word 2: S29GL01GS
        case 0x0F: return 0x2201;  // device ID, word 3
        default: return 0;
        }
    }
    if (state_ == State::Cfi)
        return 0;
    std::uint32_t value = 0;
    std::memcpy(&value, array_.bytes.data() + offset, std::min<std::size_t>(size, array_.bytes.size() - offset));
    return value;
}

void NorFlash::write(std::uint32_t offset, std::uint32_t value, int size) {
    if (size == 4) {  // two bus cycles on the 16-bit flash
        write(offset, value & 0xFFFF, 2);
        write(offset + 2, value >> 16, 2);
        return;
    }
    const std::uint32_t word = (offset >> 1) & 0xFFF;
    const std::uint8_t command = value & 0xFF;

    switch (state_) {
    case State::Program:
        programWord(offset, static_cast<std::uint16_t>(value));
        return setState(State::Read);
    case State::BufferCount:
        bufferWordsLeft_ = (value & 0xFFFF) + 1;
        return setState(State::BufferData);
    case State::BufferData:
        programWord(offset, static_cast<std::uint16_t>(value));
        if (--bufferWordsLeft_ == 0)
            setState(State::BufferConfirm);
        return;
    default:
        break;
    }

    if (command == 0xF0)
        return setState(State::Read);

    switch (state_) {
    case State::Read:
    case State::Autoselect:
    case State::Cfi:
        if (word == 0x555 && command == 0xAA)
            return setState(State::Unlock1);
        if (word == 0x55 && command == 0x98)
            return setState(State::Cfi);
        break;
    case State::Unlock1:
        if (word == 0x2AA && command == 0x55)
            return setState(State::Command);
        break;
    case State::Command:
        switch (command) {
        case 0xA0: return setState(State::Program);
        case 0x25: return setState(State::BufferCount);
        case 0x80: return setState(State::EraseUnlock1);
        case 0x90: return setState(State::Autoselect);
        }
        break;
    case State::BufferConfirm:
        if (command == 0x29)
            return setState(State::Read);
        break;
    case State::EraseUnlock1:
        if (word == 0x555 && command == 0xAA)
            return setState(State::EraseUnlock2);
        break;
    case State::EraseUnlock2:
        if (word == 0x2AA && command == 0x55)
            return setState(State::EraseCommand);
        break;
    case State::EraseCommand:
        if (command == 0x30) {
            erase(offset & ~(kSectorSize - 1), kSectorSize);
            return setState(State::Read);
        }
        if (word == 0x555 && command == 0x10) {
            erase(0, static_cast<std::uint32_t>(array_.bytes.size()));
            return setState(State::Read);
        }
        break;
    default:
        break;
    }
    setState(State::Read);  // an invalid sequence returns the chip to read mode
}

void NorFlash::programWord(std::uint32_t offset, std::uint16_t value) {
    offset &= ~1u;
    if (offset + 2 > array_.bytes.size())
        return;
    std::uint16_t current;
    std::memcpy(&current, array_.bytes.data() + offset, 2);
    current &= value;  // programming can only clear bits
    std::memcpy(array_.bytes.data() + offset, &current, 2);
    ++wordsProgrammed_;
    markDirty(offset, offset + 1);
}

void NorFlash::erase(std::uint32_t offset, std::uint32_t length) {
    std::fill_n(array_.bytes.begin() + offset, length, 0xFF);
    sectorsErased_ += length / kSectorSize;
    markDirty(offset, offset + length - 1);
}

void NorFlash::markDirty(std::uint32_t first, std::uint32_t last) {
    dirtyFirst_ = std::min(dirtyFirst_, first);
    dirtyLast_ = std::max(dirtyLast_, last);
}

}  // namespace hydra
