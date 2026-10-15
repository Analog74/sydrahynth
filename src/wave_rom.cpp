#include "wave_rom.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <stdexcept>

namespace hydra {

namespace {

constexpr int kWaves = 219;
constexpr int kLevels = 17;
constexpr std::size_t kSourceOffset = 0xDBD5C;
constexpr int kSourceSamples = 256;
constexpr int kCycle = 4096;
constexpr int kLevelSamples = 2 * kCycle + 16;
constexpr std::size_t kLevelBytes = kLevelSamples * 2;
constexpr std::size_t kWaveBytes = kLevels * kLevelBytes;
constexpr std::size_t kDataOffset = 0x10000;
constexpr std::size_t kRomBytes = kDataOffset + kWaves * kWaveBytes;

// Highest harmonic per level is (0x4000000 / limit) - 1, capped at 126; levels
// 0-5 always keep harmonics up to 127.
constexpr std::array<std::uint32_t, kLevels> kLevelLimit = {
    40273, 64186, 102204, 162679, 258553, 410341, 649437, 1030981, 1617225,
    2577452, 3927546, 5891319, 9164274, 13746411, 20619617, 27492823, 43662432};

struct Complex {
    double re = 0, im = 0;
};

// In-place iterative radix-2 FFT with a recursively updated twiddle factor.
// The inverse uses the positive exponent and, if normalize is set, divides by n.
void fft(std::vector<Complex>& a, bool inverse, bool normalize) {
    const std::size_t n = a.size();
    for (std::size_t i = 1, j = 0; i < n; ++i) {
        std::size_t bit = n;
        do {
            bit >>= 1;
            j ^= bit;
        } while (!(j & bit));
        if (i < j)
            std::swap(a[i], a[j]);
    }
    for (std::size_t len = 2; len <= n; len <<= 1) {
        const double angle = (inverse ? 1.0 : -1.0) * (6.283185307179586 / static_cast<double>(len));
        const double c = std::cos(angle), s = std::sin(angle);
        for (std::size_t i = 0; i < n; i += len) {
            double wr = 1.0, wi = 0.0;
            for (std::size_t j = 0; j < len / 2; ++j) {
                Complex& u = a[i + j];
                Complex& v = a[i + j + len / 2];
                const double tr = wr * v.re - wi * v.im;
                const double ti = wr * v.im + wi * v.re;
                const Complex x = u;
                u = {x.re + tr, x.im + ti};
                v = {x.re - tr, x.im - ti};
                const double nr = c * wr - s * wi;
                const double ni = c * wi + s * wr;
                wr = nr;
                wi = ni;
            }
        }
    }
    if (inverse && normalize)
        for (Complex& x : a)
            x = {x.re / static_cast<double>(n), x.im / static_cast<double>(n)};
}

// One 4096-sample cycle from harmonics 1..harmonics of a 256-point spectrum.
std::vector<Complex> synthesize(const std::vector<Complex>& spectrum, int harmonics, double gain) {
    std::vector<Complex> buffer(kCycle);
    for (int k = 1; k <= harmonics; ++k) {
        const Complex h = {spectrum[k].re * 16.0 * gain, spectrum[k].im * 16.0 * gain};
        buffer[k] = h;
        buffer[kCycle - k] = {h.re, -h.im};
    }
    return buffer;
}

void put32(std::vector<std::uint8_t>& rom, std::size_t offset, std::uint32_t value) {
    std::memcpy(rom.data() + offset, &value, 4);
}

std::int16_t toSample(double v) {
    const double rounded = std::trunc(v + std::copysign(0.49999999999999994, v));
    if (rounded < -32768.0)
        return -32768;
    return static_cast<std::int16_t>(std::min(32767.0, rounded));
}

}  // namespace

std::vector<std::uint8_t> buildWaveRom(const FirmwarePack& firmware) {
    const FirmwareSection* code = firmware.find("A5 code");
    if (!code || code->data.size() < kSourceOffset + kWaves * kSourceSamples * 2)
        throw std::runtime_error("firmware without the wave table");

    std::vector<std::vector<Complex>> spectra(kWaves);
    for (int w = 0; w < kWaves; ++w) {
        std::vector<Complex>& s = spectra[w];
        s.resize(kSourceSamples);
        for (int i = 0; i < kSourceSamples; ++i) {
            std::int16_t sample;
            std::memcpy(&sample, code->data.data() + kSourceOffset + w * kSourceSamples * 2 + i * 2, 2);
            s[i].re = sample;
        }
        fft(s, false, false);
    }

    double peak = 1.0;
    for (const auto& spectrum : spectra) {
        std::vector<Complex> cycle = synthesize(spectrum, 127, 1.0);
        fft(cycle, true, true);
        for (const Complex& x : cycle)
            peak = std::fmax(std::fabs(x.re), peak);
    }
    const double gain = 16000.0 / peak;

    std::vector<std::uint8_t> rom(kRomBytes);
    put32(rom, 0x04, 0x12345670);
    put32(rom, 0x08, 0x8000);
    put32(rom, 0x0C, 220);
    put32(rom, 0x20, 770);
    for (std::size_t offset = 0x24; offset < 0x420; offset += 4)
        put32(rom, offset, 0x300);
    put32(rom, 0xC00, 0x88888888);
    put32(rom, 0x12E0, 0x88888888);
    for (std::uint32_t w = 0; w < kWaves; ++w) {
        put32(rom, 0xC08 + 8 * w, w);
        put32(rom, 0xC0C + 8 * w, 1210 + 2 * w);
        put32(rom, 0x12E8 + 8 * w, 0x7FFF0000);
        put32(rom, 0x12EC + 8 * w, 1648 + 4 * w);
        put32(rom, 0x19C0 + 16 * w, 0x22220000 | w);
        put32(rom, 0x19C4 + 16 * w, 0x33330000 | w);
        put32(rom, 0x19C8 + 16 * w, static_cast<std::uint32_t>(kDataOffset + w * kWaveBytes));
        put32(rom, 0x19CC + 16 * w, 0x44440000 | w);
    }

    for (int w = 0; w < kWaves; ++w) {
        for (int level = 0; level < kLevels; ++level) {
            const std::uint32_t quotient = 0x4000000 / kLevelLimit[level];
            int harmonics = std::min<int>(quotient == 0 ? 0 : quotient - 1, 126) + 1;
            if (level < 6)
                harmonics = 127;
            std::vector<Complex> cycle = synthesize(spectra[w], harmonics, gain);
            fft(cycle, true, false);
            std::uint8_t* out = rom.data() + kDataOffset + w * kWaveBytes + level * kLevelBytes;
            for (int i = 0; i < kLevelSamples; ++i) {
                const std::int16_t sample = toSample(cycle[i & (kCycle - 1)].re * (1.0 / 4096.0));
                std::memcpy(out + 2 * i, &sample, 2);
            }
        }
    }
    return rom;
}

}  // namespace hydra
