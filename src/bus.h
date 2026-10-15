#pragma once

#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace hydra {

// A memory-mapped peripheral. Offsets are relative to the device's base address;
// size is the access width in bytes (1, 2 or 4).
class Device {
public:
    virtual ~Device() = default;
    virtual std::string_view name() const = 0;
    virtual std::uint32_t read(std::uint32_t offset, int size) = 0;
    virtual void write(std::uint32_t offset, std::uint32_t value, int size) = 0;
};

// Plain RAM/ROM regions that the CPU can access directly.
class Memory {
public:
    struct Region {
        std::string name;
        std::uint32_t base;
        std::vector<std::uint8_t> bytes;
        bool readOnly = false;  // reads are direct; writes go to the MMIO bus (e.g. flash commands)
        bool direct = true;     // false: all accesses go to the MMIO bus (e.g. flash in ID mode)
    };

    Region& addRam(std::string name, std::uint32_t base, std::uint32_t size, std::uint8_t fill = 0);
    // The directly accessible region containing all of [addr, addr + length), if any.
    Region* regionFor(std::uint32_t addr, std::uint32_t length);
    // Host pointer for [addr, addr + length) if it lies entirely within one region.
    std::uint8_t* hostPtr(std::uint32_t addr, std::uint32_t length);
    void load(std::uint32_t addr, const void* data, std::size_t length);
    const std::deque<Region>& regions() const { return regions_; }
    std::deque<Region>& regions() { return regions_; }

private:
    std::deque<Region> regions_;  // stable addresses
};

// Records accesses the emulator does not model yet, for bring-up diagnostics.
struct AccessLog {
    struct Entry {
        std::string device;
        std::uint32_t addr;
        bool write;
        std::uint64_t count = 0;
        std::uint32_t firstPc = 0;
        std::uint32_t lastValue = 0;
    };
    std::map<std::pair<std::uint32_t, bool>, Entry> entries;
    std::uint32_t currentPc = 0;
    bool all = false;  // also record accesses to modelled devices

    void record(std::string_view device, std::uint32_t addr, bool write, std::uint32_t value);
};

class MmioBus {
public:
    void map(std::uint32_t base, std::uint32_t size, Device* device);
    std::uint32_t read(std::uint32_t addr, int size);
    void write(std::uint32_t addr, std::uint32_t value, int size);
    AccessLog& log() { return log_; }

    // Diagnostic: accesses and time (TSC cycles, including anything the device
    // does, such as running another core) per device, while `profiling` is set.
    struct DeviceStats {
        std::uint64_t reads = 0, writes = 0, cycles = 0;
    };
    bool profiling = false;
    const std::map<const Device*, DeviceStats>& stats() const { return stats_; }

private:
    struct Mapping {
        std::uint32_t base, size;
        Device* device;
    };
    const Mapping* find(std::uint32_t addr) const;

    std::vector<Mapping> mappings_;  // sorted by base
    mutable const Mapping* last_ = nullptr;  // most recent hit; accesses come in runs
    AccessLog log_;
    std::map<const Device*, DeviceStats> stats_;
};

// Register file that remembers writes and logs every access. Stands in for
// peripherals that have not been modelled yet.
class StubDevice : public Device {
public:
    StubDevice(std::string name, AccessLog& log, std::uint32_t base) : name_(std::move(name)), log_(log), base_(base) {}
    std::string_view name() const override { return name_; }
    std::uint32_t read(std::uint32_t offset, int size) override;
    void write(std::uint32_t offset, std::uint32_t value, int size) override;

private:
    std::string name_;
    AccessLog& log_;
    std::uint32_t base_;
    std::map<std::uint32_t, std::uint32_t> regs_;
};

}  // namespace hydra
