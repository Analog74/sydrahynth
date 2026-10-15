#pragma once

#include <cstdint>
#include <vector>

#include "bus.h"

namespace hydra {

class SpiDevice {
public:
    virtual ~SpiDevice() = default;
    virtual void select() = 0;
    virtual void deselect() = 0;
    // Full duplex: shifts `out` in and returns the byte shifted out.
    virtual std::uint8_t transfer(std::uint8_t out) = 0;
};

// Winbond W25Q128 serial NOR flash (16 MiB, JEDEC ID EF 40 18). The status
// register always reports ready; program and erase take effect immediately.
//   02 page program (wraps within the 256-byte page)   03 read   0B fast read
//   05 status   06 write enable   04 write disable   35 status 2 (reads 0)
//   20 / 52 / D8 erase 4 KiB / 32 KiB / 64 KiB   60 / C7 chip erase
//   90 / AB manufacturer and device ID   9F JEDEC ID
class SpiFlash : public SpiDevice {
public:
    static constexpr std::uint32_t kJedecId = 0xEF4018;

    explicit SpiFlash(std::uint32_t size) : data_(size, 0xFF) {}

    void select() override;
    void deselect() override;
    std::uint8_t transfer(std::uint8_t out) override;

    std::vector<std::uint8_t>& data() { return data_; }
    std::uint64_t bytesProgrammed() const { return programmed_; }
    std::uint64_t bytesRead() const { return read_; }

private:
    void erase(std::uint32_t address, std::uint32_t length);

    std::vector<std::uint8_t> data_;
    std::vector<std::uint8_t> command_;  // first 8 bytes of the current transaction
    std::uint64_t index_ = 0;            // bytes transferred in this transaction
    std::uint32_t address_ = 0;
    bool writeEnabled_ = false;
    std::uint64_t programmed_ = 0, read_ = 0;
};

// SPI controller at 0xF3000000 with the SPI flash as its device.
//   +0x00  write: transmit a byte; read (receive mode, count > 0): clock out
//          0xFF and return the received byte, decrementing the count
//   +0x04  control: bit 12 set deasserts chip select
//   +0x08  receive count (reads back only in receive mode)
//   +0x0C  plain register
//   +0x10  mode: (value & 0x180) == 0x100 is receive mode
//   +0x14  receive FIFO level: min(count, 16) in receive mode
class A5Spi : public Device {
public:
    explicit A5Spi(SpiDevice& device) : device_(device) {}
    std::string_view name() const override { return "spi"; }
    std::uint32_t read(std::uint32_t offset, int size) override;
    void write(std::uint32_t offset, std::uint32_t value, int size) override;

private:
    bool receiveMode() const { return (mode_ & 0x180) == 0x100; }

    SpiDevice& device_;
    bool selected_ = false;
    std::uint32_t control_ = 0, count_ = 0, reg0c_ = 0, mode_ = 0;
};

}  // namespace hydra
