#include "fx_dsp.h"

#include <cstdio>
#include <cstdlib>

namespace hydra {

namespace {
constexpr std::uint32_t kWords = 0x1000;
constexpr std::uint32_t kRegisterBase = 0xFE0;
constexpr std::uint32_t kDelayWords = 0x200000;
}  // namespace

FxDsp::FxDsp(std::string name)
    : name_(std::move(name)), program_(kWords), data_(kWords), io_(0x10000), delay_(kDelayWords) {
    dsp::Host& host = *this;
    core_ = std::make_unique<dsp::DspCore>(
        0, host, dsp::DspCore::Memory{program_.data(), data_.data(), kWords - 1, nullptr, kRegisterBase});
    core_->setJumpIdleDetection(true);
}

void FxDsp::start(bool read) {
    byteIndex_ = 0;  // the word address persists, so a read can follow an address write
    reading_ = read;
}

void FxDsp::stop() {
    // HYDRA_FX_LOG: one line per write transaction (start address, words).
    static const bool log = std::getenv("HYDRA_FX_LOG") != nullptr;
    if (log && !reading_ && byteIndex_ >= 2)
        std::fprintf(stderr, "[fx-i2c] %s %04x +%u words\n", name_.c_str(), transactionStart_, (byteIndex_ - 2) / 4);
}

bool FxDsp::write(std::uint8_t byte) {
    if (byteIndex_ == 0) {
        address_ = byte;
    } else if (byteIndex_ == 1) {
        address_ = (address_ | byte << 8) & 0x7FFF;
        transactionStart_ = address_;
    } else {
        const std::uint32_t lane = (byteIndex_ - 2) & 3;
        word_ = lane == 0 ? byte : word_ | std::uint32_t(byte) << (8 * lane);
        if (lane == 3)
            store(address_++, word_);
    }
    ++byteIndex_;
    return true;
}

std::uint8_t FxDsp::read() {
    const std::uint32_t lane = byteIndex_ & 3;
    const std::uint8_t byte = static_cast<std::uint8_t>(load(address_) >> (8 * lane));
    ++byteIndex_;
    if (lane == 3)
        ++address_;
    return byte;
}

std::uint32_t FxDsp::load(std::uint32_t address) const {
    if (address < kWords)
        return data_[address];
    if (address < 2 * kWords)
        return program_[address - kWords];
    if (address == 0x2000)
        return running_ ? 1 : 0;
    return 0;
}

void FxDsp::store(std::uint32_t address, std::uint32_t value) {
    ++wordsWritten_;
    if (address < kWords) {
        data_[address] = value;
    } else if (address < 2 * kWords) {
        program_[address - kWords] = value;
        core_->invalidate(address - kWords);
    } else if (address == 0x2000) {
        const bool run = value & 1;
        if (run && !running_) {
            ++programStarts_;
            core_->reset();
            ports_.fill(0);
        }
        running_ = run;
    }
}

void FxDsp::process(std::uint32_t in2, std::uint32_t in3, std::uint32_t in4, std::uint32_t in5) {
    ports_[2] = in2;
    ports_[3] = in3;
    ports_[4] = in4;
    ports_[5] = in5;
    if (!running())
        return;
    core_->raiseInterrupt(2);
    core_->run(kInstructionsPerSample);
}

std::uint32_t FxDsp::ioRead(int, std::uint32_t address) {
    address &= 0xFFFF;
    return address < kWords ? program_[address] : io_[address];
}

void FxDsp::ioWrite(int, std::uint32_t address, std::uint32_t value) {
    address &= 0xFFFF;
    if (address < kWords) {
        program_[address] = value;
        core_->invalidate(address);
    } else {
        io_[address] = value;
    }
}

void FxDsp::dataRegWrite(int, std::uint32_t address, std::uint32_t value) {
    const std::uint32_t index = address & 0x1F;
    registers_[index] = value;
    if (index == 0)
        delay_[value & (kDelayWords - 1)] = registers_[2];
    else if (index == 1)
        registers_[3] = delay_[value & (kDelayWords - 1)];
}

}  // namespace hydra
