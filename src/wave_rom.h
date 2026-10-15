#pragma once

#include <cstdint>
#include <vector>

#include "a5pack.h"

namespace hydra {

// Builds the wave ROM the tone generator plays from. It is loaded into NOR
// flash at offset kWaveRomFlashOffset.
//
// Source: 219 single-cycle waveforms of 256 signed 16-bit samples stored in
// "A5 code" at offset 0xDBD5C (firmware 2.2.0). Each wave is rendered at 17
// levels (pitch ranges), each band-limited to the harmonics that stay below
// Nyquist in that range, as a 4096-sample cycle. The whole set is scaled so the
// loudest full-band cycle peaks at 16000.
//
// Layout (byte offsets, little endian):
//   0x0004  u32 0x12345670, u32 0x8000       0x000C  u32 220
//   0x0020  u32 770, then u32 0x300 at 0x0024-0x041F
//   0x0C00  u32 0x88888888
//   0x0C08  per wave w: u32 w, u32 1210 + 2w
//   0x12E0  u32 0x88888888
//   0x12E8  per wave w: u32 0x7FFF0000, u32 1648 + 4w
//   0x19C0  per wave w: u32 0x22220000|w, 0x33330000|w, data offset, 0x44440000|w
//   0x10000 wave data: per wave 17 levels of 8208 samples (the 4096-sample
//           cycle twice, then its first 16 samples again), 279072 bytes per wave
std::vector<std::uint8_t> buildWaveRom(const FirmwarePack& firmware);

constexpr std::uint32_t kWaveRomFlashOffset = 0x200000;

}  // namespace hydra
