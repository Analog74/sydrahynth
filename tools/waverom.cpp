// Writes the wave ROM built from a firmware pack (for inspection and tests).
//
//   waverom <firmware.dat> <out.bin>

#include <cstdio>
#include <exception>
#include <fstream>

#include "a5pack.h"
#include "wave_rom.h"

int main(int argc, char** argv) {
    if (argc != 3) {
        std::fprintf(stderr, "usage: %s <firmware.dat> <out.bin>\n", argv[0]);
        return 2;
    }
    try {
        const auto rom = hydra::buildWaveRom(hydra::FirmwarePack::load(argv[1]));
        std::ofstream(argv[2], std::ios::binary).write(reinterpret_cast<const char*>(rom.data()),
                                                        static_cast<std::streamsize>(rom.size()));
        std::printf("%zu bytes\n", rom.size());
    } catch (const std::exception& e) {
        std::fprintf(stderr, "error: %s\n", e.what());
        return 1;
    }
}
