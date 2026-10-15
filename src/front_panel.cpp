#include "front_panel.h"
#include <cstdlib>
#include <cstdio>
#include <tuple>
#include <set>

#include <algorithm>
#include <cmath>

#include "machine.h"

namespace hydra {

namespace {
constexpr std::uint32_t kKeyscanBase = 0xFA000000, kKeyscanWords = 0x28;
constexpr std::uint64_t kMinPress = 40'000, kDataStep = 2'000, kEncoderStep = 10'000, kArpStep = 150'000;

// Main ADC inputs of the pads: channel per row, multiplexer position per column.
constexpr std::array<int, FrontPanel::kPadRows> kPadChannel = {3, 2, 0};
constexpr std::array<int, FrontPanel::kPadColumns> kPadMux = {0, 4, 2, 6, 1, 5, 3, 7};

// Panel ADC inputs (channel * 8 + mux).
constexpr std::array<int, FrontPanel::kKnobs> kKnobInput = {2 * 8 + 0, 2 * 8 + 4, 5 * 8 + 0};
constexpr std::array<int, FrontPanel::kArpSwitches> kArpInput = {2 * 8 + 3, 2 * 8 + 5, 2 * 8 + 7, 5 * 8 + 3};
constexpr std::array<int, FrontPanel::kArpSwitches> kArpStepValue = {0x200, 0x333, 0x2AA, 0x200};
// Wiper A of encoder e; wiper B is four multiplexer positions further.
constexpr int encoderInput(int e) {
    constexpr int kMux[4] = {0, 2, 1, 3};
    return kMux[e & 3] + 8 * (e >> 2 & 1);
}

// Encoder ring LEDs: (byte, bit) of each of the 15 LEDs per ring, in the
// active-low bitmap at panel SRAM 0x20000101.
constexpr std::uint32_t kRingBitmap = 0x20000101;
constexpr std::uint8_t kRingMap[FrontPanel::kEncoders][FrontPanel::kRingLeds][2] = {
    {{0, 0}, {0, 1}, {0, 2}, {0, 3}, {0, 4}, {0, 5}, {0, 6}, {0, 7}, {1, 0}, {1, 1}, {1, 2}, {1, 3}, {1, 4}, {1, 5}, {1, 6}},
    {{2, 7}, {3, 0}, {3, 1}, {3, 2}, {3, 3}, {3, 4}, {3, 5}, {3, 6}, {2, 0}, {2, 1}, {2, 2}, {2, 3}, {2, 4}, {2, 5}, {2, 6}},
    {{4, 7}, {5, 0}, {5, 1}, {5, 2}, {5, 3}, {5, 4}, {5, 5}, {5, 6}, {4, 0}, {4, 1}, {4, 2}, {4, 3}, {4, 4}, {4, 5}, {4, 6}},
    {{6, 0}, {6, 1}, {6, 2}, {6, 3}, {6, 4}, {6, 5}, {6, 6}, {6, 7}, {7, 0}, {7, 1}, {7, 2}, {7, 3}, {7, 4}, {7, 5}, {7, 6}},
    {{8, 0}, {8, 1}, {8, 2}, {8, 3}, {8, 4}, {8, 5}, {8, 6}, {8, 7}, {9, 0}, {9, 1}, {9, 2}, {9, 3}, {9, 4}, {9, 5}, {9, 6}},
    {{11, 0}, {11, 1}, {11, 2}, {11, 3}, {11, 4}, {11, 5}, {11, 6}, {10, 0}, {10, 1}, {10, 2}, {10, 3}, {10, 4}, {10, 5}, {10, 6}, {10, 7}},
    {{13, 0}, {13, 1}, {13, 2}, {12, 0}, {12, 1}, {12, 2}, {12, 3}, {12, 4}, {12, 5}, {12, 6}, {12, 7}, {13, 3}, {13, 4}, {13, 5}, {13, 6}},
    {{14, 0}, {14, 1}, {14, 2}, {14, 3}, {14, 4}, {14, 5}, {14, 6}, {14, 7}, {15, 0}, {15, 1}, {15, 2}, {15, 3}, {15, 4}, {15, 5}, {15, 6}},
};

// Button LED brightness bytes in main RAM.
constexpr std::uint32_t kButtonLedBytes = 0x150E19;  // LED i of the panel view's numbering (verified per button)

// WS2812 chains: the timer compare register each DMA stream feeds.
constexpr std::array<std::uint32_t, 4> kLedChainRegister = {0x4000043C, 0x40000440, 0x40012C40, 0x40000434};
constexpr int kLedBitWords = 24, kLedOneDuty = 14;
// Pad (row, column) -> (chain, LED).
constexpr std::pair<int, int> padLed(int row, int column) {
    if (row == 0)
        return column < 3 ? std::pair{2, 16 + column} : std::pair{0, 9 + column - 3};
    return {2, (row == 1 ? 8 : 0) + column};
}
}  // namespace

// The main CPU's keyscan block: the button matrix as bitmaps.
class FrontPanel::Keyscan : public Device {
public:
    explicit Keyscan(FrontPanel& panel) : panel_(panel) {}
    std::string_view name() const override { return "keyscan"; }
    // The firmware (0x14660, every few milliseconds) writes a row's register
    // index to +0 (0xB1, 0xB3 ... 0xBF for rows 0-7) and reads the row from +4,
    // of which it uses the low 8 bits (1 = pressed; button id = row * 11 +
    // column). The rows are also readable as words at +0x28 (diagnostic).
    std::uint32_t read(std::uint32_t offset, int size) override {
        std::uint32_t value = 0;
        if (offset == 4) {
            if (index_ >= kFirstRow && (index_ - kFirstRow) % 2 == 0 && (index_ - kFirstRow) / 2 < panel_.keyscan_.size())
                value = panel_.keyscan_[(index_ - kFirstRow) / 2] & 0xFF;
        } else if (offset >= kKeyscanWords && (offset - kKeyscanWords) / 4 < panel_.keyscan_.size()) {
            value = panel_.keyscan_[(offset - kKeyscanWords) / 4];
        }
        trace(offset, false, value, size);
        return value;
    }
    void write(std::uint32_t offset, std::uint32_t value, int size) override {
        if (offset == 0)
            index_ = value & 0xFF;
        trace(offset, true, value, size);
    }

private:
    static constexpr std::uint32_t kFirstRow = 0xB1;
    std::uint32_t index_ = 0;
    // Diagnostic (HYDRA_KEYSCAN_TRACE): each distinct access once.
    void trace(std::uint32_t offset, bool write, std::uint32_t value, int size) {
        static const bool enabled = std::getenv("HYDRA_KEYSCAN_TRACE") != nullptr;
        if (!enabled)
            return;
        static std::set<std::tuple<std::uint32_t, bool, std::uint32_t>> seen;
        if (seen.size() < 200 && seen.insert({offset, write, value}).second)
            std::fprintf(stderr, "keyscan %c +%03x = %08x (%d bytes) at %.3f s\n", write ? 'W' : 'R', offset, value, size,
                         static_cast<double>(panel_.now_) / 1e6);
    }
public:

private:
    FrontPanel& panel_;
};

FrontPanel::FrontPanel(Machine& machine) : machine_(machine) {
    keyscanDevice_ = std::make_unique<Keyscan>(*this);
    machine.bus().map(kKeyscanBase, 0x1000, keyscanDevice_.get());

    panelAdc_.fill(2048);
    for (const int input : kArpInput)
        setPanelAdc(input, 0);  // the switches start at their first position
    for (int e = 0; e < kEncoders; ++e)
        updateEncoderWipers(e);
    PanelMcu& panel = machine.panel();
    panel.adc1().sample = [this, &panel](int channel) -> std::uint16_t {
        const int mux = panel.gpio(2).output() >> 5 & 7;
        const int index = channel * 8 + mux;
        return index < static_cast<int>(panelAdc_.size()) ? panelAdc_[static_cast<std::size_t>(index)] : 2048;
    };
    panel.dma1().onTransfer = [this](std::uint32_t peripheral, const std::vector<std::uint32_t>& items) {
        for (std::size_t chain = 0; chain < kLedChainRegister.size(); ++chain)
            if (peripheral == kLedChainRegister[chain])
                captureLedChain(static_cast<int>(chain), items);
    };
}

FrontPanel::~FrontPanel() = default;

void FrontPanel::advance(std::uint64_t micros) {
    now_ = micros;
    while (!scheduledButtons_.empty() && scheduledButtons_.front().at <= now_) {
        const ScheduledButton event = scheduledButtons_.front();
        scheduledButtons_.erase(scheduledButtons_.begin());
        setButton(event.id, event.down);
    }
    for (int id = 0; id < kButtons; ++id) {
        std::uint64_t& at = releaseAt_[static_cast<std::size_t>(id)];
        if (at && at <= now_) {
            keyscan_[static_cast<std::size_t>(id / 11)] &= ~(1u << (id % 11));
            at = 0;
        }
    }

    if (dataPending_ && now_ >= dataNextStep_) {
        // Clockwise steps the phase down: (A,B) = 11, 01, 00, 10, ...
        const int step = dataPending_ > 0 ? -1 : 1;
        dataPending_ += step;
        dataPhase_ = (dataPhase_ + step) & 3;
        static constexpr bool kA[4] = {true, true, false, false}, kB[4] = {true, false, false, true};
        machine_.gpio().setInput(1, 1u << 6, kA[dataPhase_]);
        machine_.gpio().setInput(1, 1u << 4, kB[dataPhase_]);
        dataNextStep_ = now_ + kDataStep;
    }

    if (now_ >= encoderNextStep_) {
        bool moved = false;
        for (int e = 0; e < kEncoders; ++e) {
            int& pending = encoderPending_[static_cast<std::size_t>(e)];
            if (!pending)
                continue;
            const int delta = std::clamp(pending, -256, 256);
            pending -= delta;
            int& phase = encoderPhase_[static_cast<std::size_t>(e)];
            phase = (phase - delta) & 0x1FFF;
            updateEncoderWipers(e);
            moved = true;
        }
        if (moved)
            encoderNextStep_ = now_ + kEncoderStep;
    }

    if (now_ >= arpNextStep_) {
        bool moved = false;
        for (int i = 0; i < kArpSwitches; ++i) {
            int& position = arpPosition_[static_cast<std::size_t>(i)];
            const int target = arpTarget_[static_cast<std::size_t>(i)];
            if (position == target)
                continue;
            position += position < target ? 1 : -1;
            setPanelAdc(kArpInput[static_cast<std::size_t>(i)],
                        static_cast<std::uint16_t>(kArpStepValue[static_cast<std::size_t>(i)] * position & 0xFFF));
            moved = true;
        }
        if (moved)
            arpNextStep_ = now_ + kArpStep;
    }
}

void FrontPanel::setButton(int id, bool down, std::uint64_t afterMicros) {
    if (id < 0 || id >= kButtons)
        return;
    if (afterMicros) {
        const ScheduledButton event{now_ + afterMicros, id, down};
        scheduledButtons_.insert(std::upper_bound(scheduledButtons_.begin(), scheduledButtons_.end(), event,
                                                  [](const ScheduledButton& a, const ScheduledButton& b) { return a.at < b.at; }),
                                 event);
        return;
    }
    const auto i = static_cast<std::size_t>(id);
    if (down) {
        keyscan_[i / 11] |= 1u << (id % 11);
        pressedAt_[i] = now_;
        releaseAt_[i] = 0;
    } else if (keyscan_[i / 11] >> (id % 11) & 1) {
        releaseAt_[i] = std::max(now_, pressedAt_[i] + kMinPress) | 1;  // nonzero: pending
    }
}

void FrontPanel::turnDataEncoder(int detents) { dataPending_ += 2 * detents; }

void FrontPanel::turnEncoder(int index, int detents) {
    if (index >= 0 && index < kEncoders)
        encoderPending_[static_cast<std::size_t>(index)] += 64 * detents;
}

void FrontPanel::setKnob(Knob knob, float value) {
    if (knob < 0 || knob >= kKnobs)
        return;
    const float v = std::clamp(value, 0.0f, 1.0f);
    setPanelAdc(kKnobInput[static_cast<std::size_t>(knob)], static_cast<std::uint16_t>(std::lround(8 + v * (4087 - 8))));
}

void FrontPanel::setArpSwitch(ArpSwitch control, int position) {
    if (control < 0 || control >= kArpSwitches)
        return;
    const auto i = static_cast<std::size_t>(control);
    arpTarget_[i] = std::clamp(position, 0, kArpPositions[i] - 1);
}

void FrontPanel::setPad(int row, int column, float pressure) {
    if (row < 0 || row >= kPadRows || column < 0 || column >= kPadColumns)
        return;
    const std::uint32_t value =
        pressure <= 0 ? 0 : static_cast<std::uint32_t>(std::lround(std::min(pressure, 1.0f) * 773 + 250));
    machine_.adc().inputs()[static_cast<std::size_t>(kPadChannel[static_cast<std::size_t>(row)] +
                                                     8 * kPadMux[static_cast<std::size_t>(column)])] = value;
}

void FrontPanel::updateEncoderWipers(int e) {
    const int phase = encoderPhase_[static_cast<std::size_t>(e)];
    int a, b;
    if (phase < 0x800) {
        a = 2048 + phase;
        b = phase;
    } else if (phase < 0x1000) {
        a = 6144 - phase;
        b = phase;
    } else if (phase < 0x1800) {
        a = 6144 - phase;
        b = 8192 - phase;
    } else {
        a = phase - 6144;
        b = 8192 - phase;
    }
    const int input = encoderInput(e);
    setPanelAdc(input, static_cast<std::uint16_t>(std::clamp(a, 0, 4095)));
    setPanelAdc(input + 4, static_cast<std::uint16_t>(std::clamp(b, 0, 4095)));
}

void FrontPanel::captureLedChain(int chain, const std::vector<std::uint32_t>& items) {
    std::vector<std::uint8_t>& bytes = ledChains_[static_cast<std::size_t>(chain)];
    bytes.assign(items.size() / 8, 0);
    for (std::size_t i = 0; i < bytes.size() * 8; ++i)
        if (items[i] >= kLedOneDuty)
            bytes[i / 8] |= static_cast<std::uint8_t>(0x80 >> (i % 8));
}

void FrontPanel::readLights(Lights& out) const {
    PanelMcu& panel = machine_.panel();
    for (int d = 0; d < PanelDisplays::kCount; ++d)
        panel.displays().display(d).render(out.displays[static_cast<std::size_t>(d)]);

    if (const std::uint8_t* leds = machine_.memory().hostPtr(kButtonLedBytes, kButtonLeds))
        for (int i = 0; i < kButtonLeds; ++i)
            out.buttons[static_cast<std::size_t>(i)] = std::clamp((leds[i] - 16) / 184.0f, 0.0f, 1.0f);

    std::uint8_t bitmap[16];
    for (std::uint32_t i = 0; i < 16; ++i)
        bitmap[i] = static_cast<std::uint8_t>(panel.core().read(kRingBitmap + i, 1));
    for (int e = 0; e < kEncoders; ++e)
        for (int l = 0; l < kRingLeds; ++l) {
            const auto& [byte, bit] = kRingMap[e][l];
            out.rings[static_cast<std::size_t>(e)][static_cast<std::size_t>(l)] = !(bitmap[byte] >> bit & 1);
        }

    // The patch knob's ring: LEDs 0-3 of chain 1, all set to the patch's colour.
    if (const std::vector<std::uint8_t>& ring = ledChains_[1]; ring.size() >= 3)
        out.patchRing = static_cast<std::uint32_t>(ring[1]) << 16 | static_cast<std::uint32_t>(ring[0]) << 8 | ring[2];

    for (int row = 0; row < kPadRows; ++row)
        for (int column = 0; column < kPadColumns; ++column) {
            const auto [chain, led] = padLed(row, column);
            const std::vector<std::uint8_t>& bytes = ledChains_[static_cast<std::size_t>(chain)];
            std::uint32_t rgb = 0;
            if (static_cast<std::size_t>(3 * led + 2) < bytes.size()) {
                const std::uint8_t* grb = &bytes[static_cast<std::size_t>(3 * led)];
                rgb = static_cast<std::uint32_t>(grb[1]) << 16 | static_cast<std::uint32_t>(grb[0]) << 8 | grb[2];
            }
            out.pads[static_cast<std::size_t>(row * kPadColumns + column)] = rgb;
        }
}

}  // namespace hydra
