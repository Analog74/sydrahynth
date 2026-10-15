#include "a5_peripherals.h"

#include <algorithm>

namespace hydra {

std::uint32_t A5Intc::read(std::uint32_t offset, int) {
    switch (offset) {
    case 0x04: return enable_;
    case 0x0C: return reg0c_;
    case 0x10: return reg10_;
    case 0x14: return pending_ & enable_;
    default: return 0;
    }
}

void A5Intc::write(std::uint32_t offset, std::uint32_t value, int) {
    switch (offset) {
    case 0x04: enable_ = value; break;
    case 0x08: pending_ &= ~value; break;
    case 0x0C: reg0c_ = value; break;
    case 0x10: reg10_ = value; break;
    default: break;
    }
}

void A5Intc::raise(int line) {
    pending_ |= 1u << line;
    ++raised_[line];
}

std::uint32_t A5Timer::read(std::uint32_t offset, int) {
    if (offset < 0x60) {
        const Channel& channel = channels_[offset >> 4];
        switch (offset & 0xF) {
        case 0x0: return channel.control;
        case 0x4: return channel.period;
        case 0x8: return channel.reg8;
        case 0xC: return channel.running() && channel.nextExpiry > now_ ? static_cast<std::uint32_t>(channel.nextExpiry - now_) : 0;
        }
    }
    if (offset == 0x60)
        return reg60_;
    if (offset == 0x64)
        return reg64_;
    return 0;
}

void A5Timer::write(std::uint32_t offset, std::uint32_t value, int) {
    if (offset < 0x60) {
        Channel& channel = channels_[offset >> 4];
        switch (offset & 0xF) {
        case 0x0: {
            const bool wasEnabled = channel.control & 1;
            channel.control = value;
            if ((value & 1) && !wasEnabled)
                channel.nextExpiry = now_ + channel.period;
            break;
        }
        case 0x4: channel.period = value; break;
        case 0x8: channel.reg8 = value; break;
        }
        return;
    }
    if (offset == 0x60)
        reg60_ = value;
    else if (offset == 0x64)
        reg64_ = value;
}

void A5Timer::advanceTo(std::uint64_t now) {
    if (now <= now_)
        return;
    now_ = now;
    for (std::size_t i = 0; i < channels_.size(); ++i) {
        Channel& channel = channels_[i];
        if (!channel.running() || now_ < channel.nextExpiry)
            continue;
        // Expiries missed in between collapse into one interrupt, as on a level-triggered line.
        const std::uint64_t missed = (now_ - channel.nextExpiry) / channel.period;
        channel.nextExpiry += channel.period * (missed + 1);
        intc_.raise(static_cast<int>(2 + i));
    }
}

std::optional<std::uint64_t> A5Timer::ticksToNextEvent() const {
    std::optional<std::uint64_t> next;
    for (const Channel& channel : channels_) {
        if (!channel.running())
            continue;
        const std::uint64_t ticks = channel.nextExpiry > now_ ? channel.nextExpiry - now_ : 0;
        if (!next || ticks < *next)
            next = ticks;
    }
    return next;
}

std::uint32_t A5Uart::status() const {
    const std::uint32_t count = static_cast<std::uint32_t>(std::min<std::size_t>(rx_.size(), 31));
    return count << 4 | (rx_.empty() ? 0 : 0x0C);
}

void A5Uart::updateIrq() {
    if (status() & enable_ & 0x0C)
        intc_.raise(9 + 2 * index_);
}

void A5Uart::receive(std::uint8_t byte) {
    rx_.push_back(byte);
    ++received_;
    updateIrq();
}

std::uint32_t A5Uart::read(std::uint32_t offset, int) {
    switch (offset) {
    case 0x00: {
        if (rx_.empty())
            return 0;
        const std::uint8_t byte = rx_.front();
        rx_.pop_front();
        updateIrq();
        return byte;
    }
    case 0x04: return line_;
    case 0x08: return enable_;
    case 0x0C: return status();
    case 0x10: return divisor_;
    default: return 0;
    }
}

void A5Uart::write(std::uint32_t offset, std::uint32_t value, int) {
    switch (offset) {
    case 0x00:
        ++sent_;
        if (monitor)
            monitor(static_cast<std::uint8_t>(value));
        if (onTransmit)
            onTransmit(static_cast<std::uint8_t>(value));
        break;
    case 0x04: line_ = value; break;
    case 0x08:
        enable_ = value;
        updateIrq();
        break;
    case 0x10: divisor_ = value; break;
    default: break;
    }
}

std::uint32_t A5Adc::read(std::uint32_t offset, int) {
    if (offset >= 0x04 && offset < 0x24) {
        busy_ = false;
        const std::uint32_t mux = muxSelect_ ? muxSelect_() & 7 : 0;
        return inputs_[mux * 8 + (offset - 0x04) / 4] | 0x400;
    }
    switch (offset) {
    case 0x80: return reg80_;
    case 0x84:
        if (busy_ && onBusyPoll)
            onBusyPoll();
        return (control_ & ~1u) | (busy_ ? 1 : 0);
    case 0x88: return reg88_;
    case 0x8C: return reg8c_;
    default: return 0;
    }
}

void A5Adc::write(std::uint32_t offset, std::uint32_t value, int) {
    switch (offset) {
    case 0x80: reg80_ = value; break;
    case 0x84:
        control_ = value;
        if (value & 1) {
            busy_ = true;
            if (onStart)
                onStart();
            else
                complete();
        }
        break;
    case 0x88: reg88_ = value; break;
    case 0x8C: reg8c_ = value; break;
    default: break;
    }
}

std::uint32_t A5Gpio::read(std::uint32_t offset, int) {
    if (offset >= kPorts * 0xC)
        return 0;
    const int port = offset / 0xC;
    switch (offset % 0xC) {
    case 0x0: return data_[port];
    case 0x4: return direction_[port];
    default:
        if (onInputRead)
            onInputRead(port);
        return inputs_[port];
    }
}

void A5Gpio::write(std::uint32_t offset, std::uint32_t value, int) {
    if (offset >= kPorts * 0xC)
        return;
    const int port = offset / 0xC;
    switch (offset % 0xC) {
    case 0x0: data_[port] = value; break;
    case 0x4: direction_[port] = value; break;
    default: return;  // inputs are read only
    }
    if (onOutput)
        onOutput(port);
}

}  // namespace hydra
