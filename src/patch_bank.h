#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace hydra {

struct Patch {
    int slot = 0;  // 0-127 within the bank
    std::string name;
    std::vector<std::uint8_t> body;  // 1762 (1.x) or 2786 (2.x) bytes
};

// Reads a Hydrasynth Manager bank (.hydra, a ZIP of "<NN><name>.patch" files
// stored without compression, NN the slot in hex) or a single .patch file.
std::vector<Patch> loadPatches(const std::filesystem::path& path);

}  // namespace hydra
