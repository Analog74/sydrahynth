#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

// Hydrasynth SysEx, as used by ASM's Hydrasynth Manager.
//
// Every message is F0 00 20 2B 00 6F <base64(crc ++ payload)> F7, where crc is
// the bit-inverted CRC-32 (IEEE) of the payload, little endian, and base64 is the
// standard alphabet with '=' padding.
//
// Patch upload (body: 1762 bytes for 1.x patches, 2786 for 2.x):
//   18 00                                 start
//   16 00 <index> <count> <128 bytes>     chunks of 06 00 <bank> <slot> <body>
//   14 00                                 save to memory (optional)
//   1A 00                                 end
// The instrument acknowledges messages with its own SysEx on MIDI out. Firmware
// 2.2.0 replies 19 to the start, 17 to each chunk but the last (07: patch
// received), 1B to the end and nothing to the save; it ignores the one-byte
// forms 18, 14 and 1A.
namespace hydra::sysex {

std::uint32_t crc32(const std::uint8_t* data, std::size_t length);
std::vector<std::uint8_t> encode(const std::vector<std::uint8_t>& payload);
// The payload of a message, or nullopt if it is not a valid Hydrasynth message.
std::optional<std::vector<std::uint8_t>> decode(const std::vector<std::uint8_t>& message);

std::vector<std::vector<std::uint8_t>> patchUpload(const std::vector<std::uint8_t>& body, int bank, int slot, bool save);

}  // namespace hydra::sysex
