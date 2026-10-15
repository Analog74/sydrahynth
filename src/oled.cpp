#include "oled.h"

namespace hydra {

namespace {
// Argument bytes that follow a command byte (0 for single-byte commands).
int argumentCount(std::uint8_t command) {
    switch (command) {
    case 0x20: case 0x81: case 0xA8: case 0xD3:  // addressing mode, contrast, multiplex, offset
    case 0x8D: case 0xAD: case 0xD5: case 0xD8: case 0xD9: case 0xDA: case 0xDB: case 0xDC:
        return 1;
    case 0x21: case 0x22: case 0xA3:  // column range, page range, vertical scroll area
        return 2;
    case 0x91:
        return 4;
    case 0x29: case 0x2A:
        return 5;
    case 0x26: case 0x27:
        return 6;
    default:
        return 0;
    }
}
}  // namespace

void Oled::command(std::uint8_t byte) {
    pending_[static_cast<std::size_t>(pendingCount_++)] = byte;
    if (pendingCount_ == 1)
        pendingNeeded_ = 1 + argumentCount(byte);
    if (pendingCount_ < pendingNeeded_)
        return;
    execute();
    pendingCount_ = 0;
    ++generation_;
}

void Oled::execute() {
    const std::uint8_t c = pending_[0];
    if (c <= 0x0F) {
        column_ = (column_ & 0xF0) | c;
    } else if (c <= 0x1F) {
        column_ = (column_ & 0x0F) | (c & 0x0F) << 4;
    } else if (c >= 0x40 && c <= 0x7F) {
        startLine_ = c & 0x3F;
    } else if (c >= 0xB0 && c <= 0xB7) {
        page_ = c & 7;
    } else {
        switch (c) {
        case 0x20: mode_ = pending_[1] & 3; break;
        case 0x21: {
            // Column range; values from 0x84 on wrap to just below 0 (relative to 132).
            const auto column = [](std::uint8_t v) { return v >= 0x84 ? (v + 124) & 0xFF : v; };
            columnStart_ = column(pending_[1]);
            columnEnd_ = column(pending_[2]);
            column_ = columnStart_;
            break;
        }
        case 0x22:
            pageStart_ = pending_[1] & 7;
            pageEnd_ = pending_[2] & 7;
            page_ = pageStart_;
            break;
        case 0x81: contrast_ = pending_[1]; break;
        case 0xA0: remap_ = false; break;
        case 0xA1: remap_ = true; break;
        case 0xA4: entireOn_ = false; break;
        case 0xA5: entireOn_ = true; break;
        case 0xA6: inverse_ = false; break;
        case 0xA7: inverse_ = true; break;
        case 0xA8: multiplex_ = pending_[1] & 0x3F; break;
        case 0xAE: on_ = false; break;
        case 0xAF: on_ = true; break;
        case 0xC0: comReverse_ = false; break;
        case 0xC8: comReverse_ = true; break;
        case 0xD3: offset_ = pending_[1] & 0x3F; break;
        default: break;
        }
    }
}

void Oled::data(std::uint8_t byte) {
    if (column_ == columnStart_ && page_ == pageStart_)
        ++frames_;
    if (column_ < kColumns && page_ < kPages)
        ram_[static_cast<std::size_t>(page_)][static_cast<std::size_t>(column_)] = byte;
    ++generation_;
    switch (mode_) {
    case 0:  // horizontal
        if (++column_ > columnEnd_) {
            column_ = columnStart_;
            if (++page_ > pageEnd_)
                page_ = pageStart_;
        }
        break;
    case 1:  // vertical
        if (++page_ > pageEnd_) {
            page_ = pageStart_;
            if (++column_ > columnEnd_)
                column_ = columnStart_;
        }
        break;
    default:  // page: the column stops at the end
        if (column_ < kColumns - 1)
            ++column_;
        break;
    }
}

void Oled::render(Image& out) const {
    const std::uint8_t lit = static_cast<std::uint8_t>(96 + 159 * contrast_ / 255);
    for (int r = 0; r < kHeight; ++r) {
        const bool visible = on_ && r <= multiplex_;
        const int row = (startLine_ + offset_ + (comReverse_ ? kHeight - 1 - r : r)) & 63;
        const auto& page = ram_[static_cast<std::size_t>(row >> 3)];
        for (int x = 0; x < kWidth; ++x) {
            const int column = columnOffset_ + (remap_ ? kWidth - 1 - x : x);
            bool pixel = false;
            if (visible) {
                const bool bit = column < kColumns && (page[static_cast<std::size_t>(column)] >> (row & 7) & 1);
                pixel = (bit || entireOn_) != inverse_;
            }
            out[static_cast<std::size_t>(r * kWidth + x)] = pixel ? lit : 0;
        }
    }
}

namespace {
// Chip-select offsets within the bank, in display order.
constexpr std::array<std::uint32_t, PanelDisplays::kCount> kDisplayBase = {0x00A00000, 0x00E00000, 0x00F00000,
                                                                         0x01E00000, 0x01F00000};
constexpr std::uint32_t kDataSelect = 0x20000;  // A17
}  // namespace

PanelDisplays::PanelDisplays() : displays_{Oled(4), Oled(2), Oled(2), Oled(2), Oled(2)} {}

void PanelDisplays::write(std::uint32_t offset, std::uint32_t value, int) {
    const std::uint32_t select = offset & 0xFFF00000;
    for (std::size_t i = 0; i < kDisplayBase.size(); ++i) {
        if (select != kDisplayBase[i])
            continue;
        if (offset & kDataSelect)
            displays_[i].data(static_cast<std::uint8_t>(value));
        else
            displays_[i].command(static_cast<std::uint8_t>(value));
        return;
    }
}

}  // namespace hydra
