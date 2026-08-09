#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace sp404 {

// The Roland-specific "RLND" chunk found in WAV files written to an SP-404SX SD card.
//
// UNVERIFIED: only `device` and `sampleIndex` are confirmed against the documented spec
// (docs/sp404sx-format.md). The four `unknownN` bytes are placeholders copied from the
// community spec's defaults and must be re-checked against the uttori-audio-wave source
// before this is used to write real files read by hardware.
struct RlndChunk {
    static constexpr size_t deviceSize = 8;
    static constexpr size_t encodedSize = deviceSize + 4 /*unknown*/ + 1 /*sampleIndex*/;

    std::array<char, deviceSize> device{'r', 'o', 'i', 'f', 's', 'p', 's', 'x'};
    uint8_t unknown1 = 0x04;
    uint8_t unknown2 = 0x00;
    uint8_t unknown3 = 0x00;
    uint8_t unknown4 = 0x00;
    uint8_t sampleIndex = 0; // 0-based: 0 == A1, +12 per bank, up to 119 == J12

    // data must point at exactly encodedSize bytes.
    static RlndChunk decode(const std::byte* data, size_t size);

    // out must point at exactly encodedSize bytes.
    void encode(std::byte* out, size_t size) const;
};

} // namespace sp404
