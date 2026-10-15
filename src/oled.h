#pragma once

#include <array>
#include <cstdint>
#include <string_view>

#include "bus.h"

namespace hydra {

// A 128x64 monochrome OLED controller with the SSD1306/SH1106 command set, as
// on the Hydrasynth's front panel: 8 pages of 132 columns of display RAM, page
// addressing by default, plus horizontal/vertical addressing, start line,
// offset, segment remap, COM scan direction, contrast, inverse and display
// on/off. Commands it does not render (charge pump, timing, scrolling) are
// consumed with their arguments.
class Oled {
public:
    static constexpr int kWidth = 128, kHeight = 64;
    using Image = std::array<std::uint8_t, kWidth * kHeight>;  // 0 = off, else brightness

    // `columnOffset`: RAM column of the first visible pixel (SH1106: 2).
    explicit Oled(int columnOffset) : columnOffset_(columnOffset) {}

    void command(std::uint8_t byte);
    void data(std::uint8_t byte);
    void render(Image& out) const;

    // Bumped on every command or data byte that can change the picture.
    std::uint64_t generation() const { return generation_; }
    // Diagnostic: data bytes written at the start of the addressing window
    // (a full redraw begins there).
    std::uint64_t frames() const { return frames_; }

private:
    void execute();

    static constexpr int kColumns = 132, kPages = 8;
    std::array<std::array<std::uint8_t, kColumns>, kPages> ram_{};
    int columnOffset_;

    // Command parser: the current command and its arguments.
    std::array<std::uint8_t, 8> pending_{};
    int pendingCount_ = 0, pendingNeeded_ = 0;

    int mode_ = 2;  // 0 horizontal, 1 vertical, 2 page
    int column_ = 0, page_ = 0;
    int columnStart_ = 0, columnEnd_ = kColumns - 1, pageStart_ = 0, pageEnd_ = kPages - 1;
    int startLine_ = 0, offset_ = 0, multiplex_ = 63, contrast_ = 0x7F;
    bool remap_ = false, comReverse_ = false, entireOn_ = false, inverse_ = false, on_ = false;
    std::uint64_t generation_ = 0, frames_ = 0;
};

// The panel MCU's FSMC bank 1 (0x60000000): the five OLEDs, each at its own
// chip select with A17 selecting data (set) or command (clear) bytes, and the
// output latches at the bottom of the bank, which are write-only here. Reads
// return 0xFF, as the panel's 8-bit bus floats high.
class PanelDisplays : public Device {
public:
    static constexpr int kCount = 5;  // 0 = main display, 1-4 = the small ones above the encoders

    PanelDisplays();
    std::string_view name() const override { return "fsmc-bank1"; }
    std::uint32_t read(std::uint32_t, int size) override { return size == 1 ? 0xFF : size == 2 ? 0xFFFF : 0xFFFFFFFF; }
    void write(std::uint32_t offset, std::uint32_t value, int size) override;

    Oled& display(int index) { return displays_[static_cast<std::size_t>(index)]; }
    const Oled& display(int index) const { return displays_[static_cast<std::size_t>(index)]; }

private:
    std::array<Oled, kCount> displays_;
};

}  // namespace hydra
