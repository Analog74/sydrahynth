#pragma once

#include <array>
#include <cstdint>
#include <deque>
#include <functional>
#include <optional>

#include "bus.h"

namespace hydra {

// Interrupt controller at 0xFF000000. Level-style pending bits are set by
// sources and cleared by writing 1s; an IRQ is requested while any enabled bit
// is pending.
//   +0x04 enable mask     +0x08 pending, write 1 to clear
//   +0x0C, +0x10 plain registers (purpose unknown)
//   +0x14 pending & enable (read only)
class A5Intc : public Device {
public:
    std::string_view name() const override { return "intc"; }
    std::uint32_t read(std::uint32_t offset, int size) override;
    void write(std::uint32_t offset, std::uint32_t value, int size) override;

    void raise(int line);
    bool irqPending() const { return (pending_ & enable_) != 0; }

    const std::array<std::uint64_t, 32>& raisedCounts() const { return raised_; }

private:
    std::uint32_t pending_ = 0, enable_ = 0, reg0c_ = 0, reg10_ = 0;
    std::array<std::uint64_t, 32> raised_{};
};

// Six periodic timer channels at 0xF1000000, clocked at 67.7376 MHz
// (1536 ticks per 44.1 kHz audio sample). Channel n raises interrupt line 2 + n.
//   +0x10*n + 0x0  control: bit 0 enables; enabling arms the first expiry
//   +0x10*n + 0x4  period in ticks
//   +0x10*n + 0x8  plain register
//   +0x10*n + 0xC  ticks until the next expiry (read only)
//   +0x60, +0x64   plain registers
class A5Timer : public Device {
public:
    static constexpr std::uint64_t kHz = 67'737'600;

    explicit A5Timer(A5Intc& intc) : intc_(intc) {}
    std::string_view name() const override { return "timer"; }
    std::uint32_t read(std::uint32_t offset, int size) override;
    void write(std::uint32_t offset, std::uint32_t value, int size) override;

    // Moves the timer clock forward, raising interrupts for every channel that expired.
    void advanceTo(std::uint64_t now);
    // Timer ticks until the next expiry of any running channel.
    std::optional<std::uint64_t> ticksToNextEvent() const;

private:
    struct Channel {
        std::uint32_t control = 0, period = 0, reg8 = 0;
        std::uint64_t nextExpiry = 0;
        bool running() const { return (control & 1) && period != 0; }
    };

    A5Intc& intc_;
    std::array<Channel, 6> channels_{};
    std::uint32_t reg60_ = 0, reg64_ = 0;
    std::uint64_t now_ = 0;
};

// UART n at 0xF5000000 + n*0x200. Transmission is instantaneous.
//   +0x00  read: pop a received byte (0 if none); write: transmit a byte
//   +0x04  line settings (stored)      +0x08  interrupt enable
//   +0x0C  status: min(rx count, 31) << 4, plus 0x0C while data is waiting
//   +0x10  baud divisor (stored)
// While data is waiting and enable & 0x0C, it raises interrupt line 9 + 2n.
// Board wiring: UART0 carries MIDI, UART1 links to the panel MCU.
class A5Uart : public Device {
public:
    A5Uart(A5Intc& intc, int index) : intc_(intc), index_(index) {}
    std::string_view name() const override { return name_[index_]; }
    std::uint32_t read(std::uint32_t offset, int size) override;
    void write(std::uint32_t offset, std::uint32_t value, int size) override;

    void receive(std::uint8_t byte);
    std::function<void(std::uint8_t byte)> onTransmit;
    std::function<void(std::uint8_t byte)> monitor;  // diagnostics: sees every transmitted byte
    std::uint64_t bytesSent() const { return sent_; }
    std::uint64_t bytesReceived() const { return received_; }

private:
    static constexpr std::string_view name_[3] = {"uart0", "uart1", "uart2"};

    std::uint32_t status() const;
    void updateIrq();

    A5Intc& intc_;
    int index_;
    std::deque<std::uint8_t> rx_;
    std::uint32_t line_ = 0, enable_ = 0, divisor_ = 0;
    std::uint64_t sent_ = 0, received_ = 0;
};

// ADC at 0xF7000000: 8 channels behind an 8-way analog multiplexer whose
// select lines are GPIO port 1 bits 23-25 (64 inputs in total).
//   +0x04 + 4c  channel c of the selected mux position: value | 0x400
//   +0x80  plain register
//   +0x84  control: writing bit 0 starts a conversion; when it completes
//          (complete(), timed by the machine) it raises interrupt line 27;
//          reads back with bit 0 = busy (cleared by reading a channel)
//   +0x88, +0x8C  plain registers
class A5Adc : public Device {
public:
    A5Adc(A5Intc& intc, std::function<std::uint32_t()> muxSelect) : intc_(intc), muxSelect_(std::move(muxSelect)) {}
    std::string_view name() const override { return "adc"; }
    std::uint32_t read(std::uint32_t offset, int size) override;
    void write(std::uint32_t offset, std::uint32_t value, int size) override;

    std::array<std::uint32_t, 64>& inputs() { return inputs_; }
    // Ends the running conversion: raises the interrupt.
    void complete() { intc_.raise(27); }
    // Called when a conversion starts, and when the CPU reads the control
    // register while busy (polling for the interrupt handler to fetch the result).
    std::function<void()> onStart, onBusyPoll;

private:
    A5Intc& intc_;
    std::function<std::uint32_t()> muxSelect_;
    std::array<std::uint32_t, 64> inputs_{};
    std::uint32_t reg80_ = 0, control_ = 0, reg88_ = 0, reg8c_ = 0;
    bool busy_ = false;
};

// Eight GPIO ports at 0xF4000000, 0xC apart:
//   +0x0 output data   +0x4 direction (1 = output)   +0x8 input levels (read only)
// Inputs idle high (pull-ups). The board wires bit-banged I2C buses to port 0;
// onOutput lets the machine follow the driven levels and feed back inputs.
class A5Gpio : public Device {
public:
    static constexpr int kPorts = 8;

    A5Gpio() { inputs_.fill(0xFFFFFFFF); }
    std::string_view name() const override { return "gpio"; }
    std::uint32_t read(std::uint32_t offset, int size) override;
    void write(std::uint32_t offset, std::uint32_t value, int size) override;

    // Level of an open-drain line: high unless the pin is an output driving 0.
    std::uint32_t output(int port) const { return data_[port]; }
    bool level(int port, std::uint32_t mask) const {
        return (data_[port] & mask) || !(direction_[port] & mask);
    }
    void setInput(int port, std::uint32_t mask, bool high) {
        inputs_[port] = high ? inputs_[port] | mask : inputs_[port] & ~mask;
    }

    std::function<void(int port)> onOutput;
    std::function<void(int port)> onInputRead;

private:
    std::array<std::uint32_t, kPorts> data_{}, direction_{}, inputs_{};
};

}  // namespace hydra
