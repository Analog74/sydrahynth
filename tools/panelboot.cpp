// Boots the panel MCU firmware on its own and reports what it touched.
//
//   panelboot <firmware.dat> [cycles, 72 MHz]

#include <cstdio>
#include <cstdlib>
#include <exception>

#include "a5pack.h"
#include "panel_mcu.h"

using namespace hydra;

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s <firmware.dat> [cycles]\n", argv[0]);
        return 2;
    }
    const std::uint64_t cycles = argc > 2 ? std::strtoull(argv[2], nullptr, 0) : PanelMcu::kHz;
    try {
        const FirmwarePack firmware = FirmwarePack::load(argv[1]);
        const FirmwareSection* code = firmware.find("stm32 code");
        if (!code)
            throw std::runtime_error("firmware has no 'stm32 code' section");
        PanelMcu panel(*code);
        panel.bus().log().all = true;
        panel.run(cycles);
        CortexM3& core = panel.core();
        std::printf("after %llu cycles%s%s\n", static_cast<unsigned long long>(core.now()),
                    core.fatalMessage().empty() ? "" : ": FATAL ", core.fatalMessage().c_str());
        const auto& r = core.regs();
        for (int i = 0; i < 16; ++i)
            std::printf("%sr%-2d=%08x", i % 4 ? "  " : "\n  ", i, r[i]);
        std::printf("\n  sleeping=%d\n\nperipheral accesses (address, R/W, count, first pc, last value):\n", core.sleeping());
        for (const auto& [where, e] : panel.bus().log().entries)
            std::printf("  %-10s %08x %c %10llu  pc=%08x  %08x\n", e.device.c_str(), e.addr, e.write ? 'W' : 'R',
                        static_cast<unsigned long long>(e.count), e.firstPc, e.lastValue);
        return core.fatalMessage().empty() ? 0 : 1;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "error: %s\n", e.what());
        return 1;
    }
}
