#pragma once

#include <cstdint>
#include <map>

namespace hydra {

class I2cSlave {
public:
    virtual ~I2cSlave() = default;
    virtual void start(bool read) = 0;
    // Returns whether the byte is acknowledged.
    virtual bool write(std::uint8_t byte) = 0;
    virtual std::uint8_t read() = 0;
    virtual void stop() = 0;
};

// Decodes a bit-banged I2C bus from its SCL/SDA levels and plays the slave
// side for the attached devices.
class I2cBus {
public:
    void attach(std::uint8_t address, I2cSlave* slave) { slaves_[address] = slave; }

    // Feeds the levels the master drives (true = released/high) and returns the
    // level the addressed slave drives on SDA.
    bool update(bool scl, bool sda);

    std::uint64_t nackedAddresses() const { return nackedAddresses_; }

private:
    enum class Phase { Idle, Address, Data };

    void finishByte();
    void release();

    std::map<std::uint8_t, I2cSlave*> slaves_;
    I2cSlave* active_ = nullptr;
    Phase phase_ = Phase::Idle;
    bool prevScl_ = true, prevSda_ = true, sdaOut_ = true;
    bool reading_ = false, masterNacked_ = false;
    int bitCount_ = 0;
    std::uint8_t shift_ = 0, readByte_ = 0xFF;
    std::uint64_t nackedAddresses_ = 0;
};

}  // namespace hydra
