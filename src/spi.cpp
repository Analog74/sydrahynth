#include "spi.h"

#include <algorithm>

namespace hydra {

void SpiFlash::select() {
    command_.clear();
    index_ = 0;
}

void SpiFlash::erase(std::uint32_t address, std::uint32_t length) {
    const std::uint32_t start = (address % data_.size()) & ~(length - 1);
    std::fill_n(data_.begin() + start, std::min<std::size_t>(length, data_.size() - start), 0xFF);
}

void SpiFlash::deselect() {
    if (command_.empty())
        return;
    const std::uint8_t command = command_[0];
    const bool hasAddress = command_.size() >= 4;
    const std::uint32_t address = hasAddress ? command_[1] << 16 | command_[2] << 8 | command_[3] : 0;
    switch (command) {
    case 0x06:
        writeEnabled_ = true;
        return;
    case 0x20:
        if (writeEnabled_ && hasAddress)
            erase(address, 0x1000);
        break;
    case 0x52:
        if (writeEnabled_ && hasAddress)
            erase(address, 0x8000);
        break;
    case 0xD8:
        if (writeEnabled_ && hasAddress)
            erase(address, 0x10000);
        break;
    case 0x60:
    case 0xC7:
        if (writeEnabled_)
            std::fill(data_.begin(), data_.end(), 0xFF);
        break;
    case 0x02:
    case 0x04:
        break;
    default:
        return;  // other commands leave the write enable latch alone
    }
    writeEnabled_ = false;
}

std::uint8_t SpiFlash::transfer(std::uint8_t out) {
    if (command_.size() < 8)
        command_.push_back(out);
    const std::uint64_t i = index_++;
    const std::uint8_t command = command_[0];
    const auto addressComplete = [&] {
        if (i == 3)
            address_ = command_[1] << 16 | command_[2] << 8 | command_[3];
    };
    switch (command) {
    case 0x02:
        addressComplete();
        if (i >= 4 && writeEnabled_) {
            const std::uint32_t at = ((address_ & 0xFFFFFF00) | ((address_ + i - 4) & 0xFF)) % data_.size();
            data_[at] &= out;
            ++programmed_;
        }
        return 0xFF;
    case 0x03:
    case 0x0B:
        addressComplete();
        if (i + 1 >= (command == 0x03 ? 5u : 6u)) {
            ++read_;
            return data_[address_++ % data_.size()];
        }
        return 0xFF;
    case 0x05: return i == 0 ? 0xFF : 0x00;
    case 0x35: return 0x00;
    case 0x90:
    case 0xAB:
        if (i + 1 < 5)
            return 0xFF;
        return (i & 1) ? kJedecId >> 16 & 0xFF : (kJedecId & 0xFF) - 1;
    case 0x9F:
        if (i == 1) return kJedecId >> 16 & 0xFF;
        if (i == 2) return kJedecId >> 8 & 0xFF;
        if (i == 3) return kJedecId & 0xFF;
        return 0xFF;
    default:
        return 0xFF;
    }
}

std::uint32_t A5Spi::read(std::uint32_t offset, int) {
    switch (offset) {
    case 0x00:
        if (!receiveMode() || count_ == 0)
            return 0xFF;
        --count_;
        return selected_ ? device_.transfer(0xFF) : 0xFF;
    case 0x04: return control_;
    case 0x08: return receiveMode() ? count_ : 0;
    case 0x0C: return reg0c_;
    case 0x10: return mode_;
    case 0x14: return receiveMode() ? std::min<std::uint32_t>(count_, 16) : 0;
    default: return 0;
    }
}

void A5Spi::write(std::uint32_t offset, std::uint32_t value, int) {
    switch (offset) {
    case 0x00:
        if (selected_)
            device_.transfer(static_cast<std::uint8_t>(value));
        break;
    case 0x04: {
        const bool select = !(value & 0x1000);
        if (select != selected_) {
            if (select)
                device_.select();
            else
                device_.deselect();
        }
        selected_ = select;
        control_ = value;
        break;
    }
    case 0x08: count_ = value; break;
    case 0x0C: reg0c_ = value; break;
    case 0x10: mode_ = value; break;
    default: break;
    }
}

}  // namespace hydra
