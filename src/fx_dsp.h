#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "dsp_core.h"
#include "i2c.h"

namespace hydra {

// One effects DSP: the same DSP core as the main block, loaded and controlled by
// the main CPU over I2C (address 0x51).
//
// I2C word address space:
//   0x0000-0x0FFF  data memory
//   0x1000-0x1FFF  program memory
//   0x2000         control; setting bit 0 resets the core and starts the program
// A transaction starts with the 15-bit word address (two bytes, low byte first);
// after that, 32-bit little-endian words are written or read with auto-increment.
//
// DSP view: data memory 0x000-0xFDF, with 32 registers at 0xFE0-0xFFF; register 0
// is the delay RAM write address (writing it stores register 2 there), register
// 1 the read address (writing it loads the word into register 3). The delay RAM
// holds 2M words. io and ext space (16 bit): 0x0000-0x0FFF is the program memory
// (as in the main block), the rest a separate 64K-word RAM. Ports 0-7: per sample, process() puts four
// input words into ports 2-5; the program leaves its results in the others.
class FxDsp : public I2cSlave, private dsp::Host {
public:
    static constexpr int kPorts = 8;
    static constexpr std::uint64_t kInstructionsPerSample = 6000;

    explicit FxDsp(std::string name);
    FxDsp(const FxDsp&) = delete;
    FxDsp& operator=(const FxDsp&) = delete;

    // I2cSlave
    void start(bool read) override;
    bool write(std::uint8_t byte) override;
    std::uint8_t read() override;
    void stop() override;

    // One sample: inputs into ports 2-5, then (if the program runs) the sample
    // interrupt and kInstructionsPerSample instructions.
    void process(std::uint32_t in2, std::uint32_t in3, std::uint32_t in4, std::uint32_t in5);
    std::uint32_t port(int index) const { return ports_[index & 7]; }

    const std::string& name() const { return name_; }
    bool running() const { return running_ && !core_->trapped(); }
    std::uint64_t wordsWritten() const { return wordsWritten_; }
    std::uint64_t starts() const { return programStarts_; }
    dsp::DspCore& core() { return *core_; }
    const dsp::DspCore& core() const { return *core_; }
    const std::vector<std::uint32_t>& program() const { return program_; }
    const std::vector<std::uint32_t>& data() const { return data_; }

private:
    std::uint32_t load(std::uint32_t address) const;
    void store(std::uint32_t address, std::uint32_t value);

    // dsp::Host
    std::uint32_t ioRead(int, std::uint32_t address) override;
    void ioWrite(int, std::uint32_t address, std::uint32_t value) override;
    std::uint32_t extRead(int core, std::uint32_t address) override { return ioRead(core, address); }
    void extWrite(int core, std::uint32_t address, std::uint32_t value) override { ioWrite(core, address, value); }
    std::uint32_t portRead(int, std::uint32_t port) override { return ports_[port & 7]; }
    void portWrite(int, std::uint32_t port, std::uint32_t value) override { ports_[port & 7] = value; }
    std::uint32_t dataRegRead(int, std::uint32_t address) override { return registers_[address & 0x1F]; }
    void dataRegWrite(int, std::uint32_t address, std::uint32_t value) override;

    std::string name_;
    std::vector<std::uint32_t> program_, data_, io_, delay_;
    std::array<std::uint32_t, 32> registers_{};
    std::array<std::uint32_t, kPorts> ports_{};
    std::unique_ptr<dsp::DspCore> core_;
    bool running_ = false;
    std::uint32_t address_ = 0, word_ = 0, transactionStart_ = 0;
    bool reading_ = false;
    std::uint32_t byteIndex_ = 0;  // within the current transaction
    std::uint64_t wordsWritten_ = 0, programStarts_ = 0;
};

}  // namespace hydra
