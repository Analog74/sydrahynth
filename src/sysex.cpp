#include "sysex.h"

#include <array>
#include <stdexcept>

namespace hydra::sysex {

namespace {

constexpr char kBase64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
constexpr std::uint8_t kHeader[] = {0xF0, 0x00, 0x20, 0x2B, 0x00, 0x6F};

const std::array<std::uint32_t, 256>& crcTable() {
    static const auto table = [] {
        std::array<std::uint32_t, 256> t{};
        for (std::uint32_t i = 0; i < 256; ++i) {
            std::uint32_t c = i;
            for (int k = 0; k < 8; ++k)
                c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            t[i] = c;
        }
        return t;
    }();
    return table;
}

int base64Value(std::uint8_t c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

}  // namespace

std::uint32_t crc32(const std::uint8_t* data, std::size_t length) {
    std::uint32_t c = 0xFFFFFFFF;
    for (std::size_t i = 0; i < length; ++i)
        c = crcTable()[(c ^ data[i]) & 0xFF] ^ (c >> 8);
    return ~c;
}

std::vector<std::uint8_t> encode(const std::vector<std::uint8_t>& payload) {
    const std::uint32_t crc = ~crc32(payload.data(), payload.size());
    std::vector<std::uint8_t> raw = {static_cast<std::uint8_t>(crc), static_cast<std::uint8_t>(crc >> 8),
                                     static_cast<std::uint8_t>(crc >> 16), static_cast<std::uint8_t>(crc >> 24)};
    raw.insert(raw.end(), payload.begin(), payload.end());

    std::vector<std::uint8_t> out(std::begin(kHeader), std::end(kHeader));
    for (std::size_t i = 0; i < raw.size(); i += 3) {
        const std::uint32_t n = raw[i] << 16 | (i + 1 < raw.size() ? raw[i + 1] << 8 : 0) | (i + 2 < raw.size() ? raw[i + 2] : 0);
        out.push_back(kBase64[n >> 18 & 63]);
        out.push_back(kBase64[n >> 12 & 63]);
        out.push_back(i + 1 < raw.size() ? kBase64[n >> 6 & 63] : '=');
        out.push_back(i + 2 < raw.size() ? kBase64[n & 63] : '=');
    }
    out.push_back(0xF7);
    return out;
}

std::optional<std::vector<std::uint8_t>> decode(const std::vector<std::uint8_t>& message) {
    if (message.size() < sizeof kHeader + 1 || message.back() != 0xF7 ||
        !std::equal(std::begin(kHeader), std::end(kHeader), message.begin()))
        return std::nullopt;
    std::vector<std::uint8_t> raw;
    std::uint32_t bits = 0;
    int count = 0;
    for (std::size_t i = sizeof kHeader; i + 1 < message.size(); ++i) {
        const int v = base64Value(message[i]);
        if (v < 0)
            continue;  // padding
        bits = bits << 6 | static_cast<std::uint32_t>(v);
        count += 6;
        if (count >= 8) {
            count -= 8;
            raw.push_back(static_cast<std::uint8_t>(bits >> count));
        }
    }
    if (raw.size() < 4)
        return std::nullopt;
    std::vector<std::uint8_t> payload(raw.begin() + 4, raw.end());
    const std::uint32_t crc = ~crc32(payload.data(), payload.size());
    const std::uint32_t stored = raw[0] | raw[1] << 8 | raw[2] << 16 | static_cast<std::uint32_t>(raw[3]) << 24;
    if (crc != stored)
        return std::nullopt;
    return payload;
}

std::vector<std::vector<std::uint8_t>> patchUpload(const std::vector<std::uint8_t>& body, int bank, int slot, bool save) {
    if ((body.size() != 1762 && body.size() != 2786) || bank < 0 || bank > 7 || slot < 0 || slot > 127)
        throw std::invalid_argument("patch upload needs a 1762- or 2786-byte body, bank 0-7 and slot 0-127");
    std::vector<std::uint8_t> data = {0x06, 0x00, static_cast<std::uint8_t>(bank), static_cast<std::uint8_t>(slot)};
    data.insert(data.end(), body.begin(), body.end());

    std::vector<std::vector<std::uint8_t>> messages = {encode({0x18, 0x00})};
    const std::size_t chunks = (data.size() + 127) / 128;
    for (std::size_t i = 0; i < chunks; ++i) {
        std::vector<std::uint8_t> chunk = {0x16, 0x00, static_cast<std::uint8_t>(i), static_cast<std::uint8_t>(chunks)};
        const auto begin = data.begin() + static_cast<std::ptrdiff_t>(i * 128);
        chunk.insert(chunk.end(), begin, begin + static_cast<std::ptrdiff_t>(std::min<std::size_t>(128, data.size() - i * 128)));
        messages.push_back(encode(chunk));
    }
    if (save)
        messages.push_back(encode({0x14, 0x00}));
    messages.push_back(encode({0x1A, 0x00}));
    return messages;
}

}  // namespace hydra::sysex
