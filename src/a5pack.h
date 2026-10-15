#pragma once

#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace hydra {

// One payload of an ASM "A5Pack" firmware container (e.g. "A5 code", "stm32 code").
struct FirmwareSection {
    std::string version;  // e.g. "220"
    std::string name;
    std::vector<std::uint8_t> data;
};

// The Hydrasynth Desktop firmware .dat file. Layout (all little endian):
//   0x00  "A5Pack\0"
//   0x07  model tag, NUL terminated ("DR" for the desktop module)
//   ...   copyright text, zero padded up to 0x5B
//   0x5B  sections until end of file, each:
//           char version[20]; char name[20]; u32 size; u8 data[size];
struct FirmwarePack {
    std::string model;
    std::vector<FirmwareSection> sections;

    static FirmwarePack parse(std::span<const std::uint8_t> bytes);
    static FirmwarePack load(const std::filesystem::path& path);

    const FirmwareSection* find(std::string_view name) const;
};

}  // namespace hydra
