// Disassembles ARM (A32) code from a raw image loaded at address 0, e.g. the
// firmware's "A5 code" section: armdis <image> <start> <end> (hex addresses).
// With --thumb <load address>, Thumb code (the panel's "stm32 code" at
// 08010000); 32-bit Thumb instructions are shown as raw halfwords.

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <vector>

#include <dynarmic/frontend/A32/disassembler/disassembler.h>

int main(int argc, char** argv) {
    std::uint32_t thumbBase = 0;
    bool thumb = false;
    if (argc >= 3 && std::strcmp(argv[1], "--thumb") == 0) {
        thumb = true;
        thumbBase = static_cast<std::uint32_t>(std::strtoul(argv[2], nullptr, 16));
        argv += 2;
        argc -= 2;
    }
    if (argc < 4) {
        std::fprintf(stderr, "usage: %s [--thumb base] <image> <start> <end>\n", argv[0]);
        return 2;
    }
    std::ifstream in(argv[1], std::ios::binary);
    const std::vector<char> image((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    const std::uint32_t start = static_cast<std::uint32_t>(std::strtoul(argv[2], nullptr, 16));
    const std::uint32_t end = static_cast<std::uint32_t>(std::strtoul(argv[3], nullptr, 16));
    if (thumb) {
        for (std::uint32_t at = start & ~1u; at < end && at - thumbBase + 2 <= image.size();) {
            std::uint16_t half;
            std::memcpy(&half, image.data() + (at - thumbBase), 2);
            if ((half >> 11) >= 0x1D) {  // 32-bit instruction
                std::uint16_t second;
                std::memcpy(&second, image.data() + (at - thumbBase + 2), 2);
                std::printf("%08x  %04x %04x  (32-bit)\n", at, half, second);
                at += 4;
            } else {
                std::printf("%08x  %04x       %s\n", at, half, Dynarmic::A32::DisassembleThumb16(half).c_str());
                at += 2;
            }
        }
        return 0;
    }
    for (std::uint32_t at = start & ~3u; at < end && at + 4 <= image.size(); at += 4) {
        std::uint32_t word;
        std::memcpy(&word, image.data() + at, 4);
        std::printf("%08x  %08x  %s\n", at, word, Dynarmic::A32::DisassembleArm(word).c_str());
    }
    return 0;
}
