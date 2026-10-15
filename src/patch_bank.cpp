#include "patch_bank.h"

#include <cstring>
#include <fstream>
#include <stdexcept>

namespace hydra {

namespace {

std::uint32_t le32(const std::uint8_t* p) { return p[0] | p[1] << 8 | p[2] << 16 | static_cast<std::uint32_t>(p[3]) << 24; }
std::uint16_t le16(const std::uint8_t* p) { return static_cast<std::uint16_t>(p[0] | p[1] << 8); }

bool validBody(std::size_t size) { return size == 1762 || size == 2786; }

}  // namespace

std::vector<Patch> loadPatches(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file)
        throw std::runtime_error("cannot open " + path.string());
    const std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());

    if (bytes.size() < 4 || le32(bytes.data()) != 0x04034B50) {  // not a ZIP: a single .patch
        if (!validBody(bytes.size()))
            throw std::runtime_error("unsupported patch file " + path.string());
        return {Patch{0, path.stem().string(), bytes}};
    }

    std::vector<Patch> patches;
    std::size_t offset = 0;
    while (offset + 30 <= bytes.size() && le32(bytes.data() + offset) == 0x04034B50) {
        const std::uint8_t* h = bytes.data() + offset;
        const std::uint16_t flags = le16(h + 6), method = le16(h + 8);
        const std::uint32_t size = le32(h + 18);
        const std::uint16_t nameLength = le16(h + 26), extraLength = le16(h + 28);
        if (flags & 0x08)
            throw std::runtime_error("ZIP entries with data descriptors are not supported");
        const std::size_t dataOffset = offset + 30 + nameLength + extraLength;
        if (dataOffset + size > bytes.size())
            throw std::runtime_error("truncated bank " + path.string());
        std::string name(reinterpret_cast<const char*>(h + 30), nameLength);
        if (method == 0 && validBody(size) && name.size() > 2 && name.ends_with(".patch")) {
            Patch patch;
            patch.slot = std::stoi(name.substr(0, 2), nullptr, 16);  // two hex digits, 00-7f
            patch.name = name.substr(2, name.size() - 2 - 6);
            patch.body.assign(bytes.begin() + static_cast<std::ptrdiff_t>(dataOffset),
                              bytes.begin() + static_cast<std::ptrdiff_t>(dataOffset + size));
            patches.push_back(std::move(patch));
        }
        offset = dataOffset + size;
    }
    return patches;
}

}  // namespace hydra
