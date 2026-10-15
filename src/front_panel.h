#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <vector>

#include "oled.h"

namespace hydra {

class Machine;

// The front panel's controls and lights, as seen by the firmware: the inputs
// a user operates (buttons, data encoder, pads, knobs, arp switches, the eight
// endless encoders) are turned into the signals the boards read, paced the way
// a hand moves them; the outputs (displays, button LEDs, encoder rings, pad
// colours) are read back from the emulated hardware.
//
// Inputs:
//   buttons        main keyscan block at 0xFA000000: row id / 11 (8 rows), read
//                  by writing 0xB1 + 2 * row to +0 and reading +4, bit id % 11
//                  (the firmware uses 8 columns), set while pressed (>= 40 ms)
//   data encoder   main GPIO port 1 inputs 6 (A) and 4 (B), quadrature, one
//                  phase step per 2 ms, two steps per detent
//   pads           main ADC inputs (channel + 8 * mux): pressure 250..1023
//   knobs, arp     panel ADC1 channels behind the GPIOC 5-7 multiplexer;
//   switches,      the switches move one detent per 150 ms, the encoders are
//   encoders       dual-wiper pots (64 units per detent, <= 256 per 10 ms)
//
// Outputs:
//   displays       the panel's five OLEDs (see oled.h)
//   patch ring     WS2812 chain 1, LEDs 0-3 (the patch's colour)
//   button LEDs    96 brightness bytes the main CPU keeps at 0x150E19 (the
//                  panel view's LED numbers; checked by pressing each button)
//   encoder rings  panel SRAM 0x20000101-0x20000110, active low
//   pad colours    WS2812 chains fed by the panel's DMA1 from timer compare
//                  registers; 24 PWM words per LED, GRB
//
// Everything runs on the emulator thread; advance() is called as emulated time
// passes (at least every millisecond).
class FrontPanel {
public:
    static constexpr int kButtons = 88, kButtonLeds = 96, kEncoders = 8, kRingLeds = 15;
    static constexpr int kPadRows = 3, kPadColumns = 8, kPads = kPadRows * kPadColumns;
    enum Knob { kCutoff, kResonance, kDriveMorph, kKnobs };
    enum ArpSwitch { kArpMode, kArpOctave, kArpDivision, kArpSwing, kArpSwitches };
    static constexpr std::array<int, kArpSwitches> kArpPositions = {8, 4, 4, 8};

    struct Lights {
        std::array<Oled::Image, PanelDisplays::kCount> displays{};
        std::array<float, kButtonLeds> buttons{};  // 0..1
        std::array<std::array<bool, kRingLeds>, kEncoders> rings{};
        std::array<std::uint32_t, kPads> pads{};  // 0xRRGGBB
        std::uint32_t patchRing = 0;                // the light around the patch knob, 0xRRGGBB
    };

    explicit FrontPanel(Machine& machine);
    ~FrontPanel();

    // Brings the paced inputs up to `micros` of emulated time.
    void advance(std::uint64_t micros);

    // Presses or releases a button now, or `afterMicros` of emulated time from
    // now (for combinations: the firmware must see SHIFT held before the other
    // button). A release keeps a press at least 40 ms long.
    void setButton(int id, bool down, std::uint64_t afterMicros = 0);
    void turnDataEncoder(int detents);  // positive: clockwise
    void turnEncoder(int index, int detents);
    void setKnob(Knob knob, float value);  // 0..1
    void setArpSwitch(ArpSwitch control, int position);
    void setPad(int row, int column, float pressure);  // 0 releases; 0..1 otherwise

    void readLights(Lights& out) const;
    // Diagnostic: the GRB bytes last sent to a WS2812 chain (0-3).
    const std::vector<std::uint8_t>& ledChain(int chain) const { return ledChains_[static_cast<std::size_t>(chain)]; }

    // The keyscan words the main CPU reads.
    std::uint32_t keyscanWord(int index) const { return keyscan_[static_cast<std::size_t>(index)]; }

private:
    class Keyscan;
    void setPanelAdc(int index, std::uint16_t value) { panelAdc_[static_cast<std::size_t>(index)] = value; }
    void updateEncoderWipers(int index);
    void captureLedChain(int chain, const std::vector<std::uint32_t>& items);

    Machine& machine_;
    std::unique_ptr<Keyscan> keyscanDevice_;
    std::uint64_t now_ = 0;  // microseconds

    std::array<std::uint32_t, (kButtons + 10) / 11> keyscan_{};
    std::array<std::uint64_t, kButtons> pressedAt_{}, releaseAt_{};
    struct ScheduledButton {
        std::uint64_t at;  // microseconds
        int id;
        bool down;
    };
    std::vector<ScheduledButton> scheduledButtons_;  // in order of `at`

    int dataPhase_ = 0, dataPending_ = 0;  // in phase steps
    std::uint64_t dataNextStep_ = 0;

    std::array<std::uint16_t, 144> panelAdc_{};  // channel * 8 + mux
    std::array<int, kEncoders> encoderPhase_{}, encoderPending_{};
    std::uint64_t encoderNextStep_ = 0;
    std::array<int, kArpSwitches> arpPosition_{}, arpTarget_{};
    std::uint64_t arpNextStep_ = 0;

    std::array<std::vector<std::uint8_t>, 4> ledChains_;  // GRB bytes per chain
};

}  // namespace hydra
