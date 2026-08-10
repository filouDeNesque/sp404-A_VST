#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace sp404 {

// The Roland-specific "RLND" chunk found in WAV files written to an SP-404SX SD card.
//
// VERIFIED (2026-08-10) against a real SP-404SX SD card: `device`, the four `unknown` bytes
// (always 0x04 0x00 0x00 0x00 on every real sample checked), and `sampleIndex` (checked against
// A1/A12/B1/J12 -- 0, 11, 12, 119, exactly matching the formula documented below) all match
// byte-for-byte. See docs/sp404sx-format.md and encodeWavWithRlndChunk()'s test fixture (a real
// file's header bytes) in core/tests/WavRlndTests.cpp. (A0000002.WAV/A0000006.WAV on the same
// card turned out to be samples this plugin itself had written during earlier testing, *without*
// an RLND chunk at all -- the very gap this function fixes, see plugin/SampleImport.cpp.)
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

// Assembles a complete little-endian 16-bit PCM WAV file with an embedded RLND chunk, matching
// the exact chunk layout of a real SP-404SX SD card sample byte-for-byte (see docs/
// sp404sx-format.md and the class comment above): RIFF/WAVE header, an 18-byte `fmt ` chunk (the
// standard 16-byte PCM fields plus a trailing 2-byte zero `cbSize` extension -- present on every
// genuine sample dumped from a real card, though not required by the WAV spec itself), then the
// RLND chunk zero-padded so the `data` chunk always starts at exactly byte offset 512 regardless
// of numChannels/sampleRate (those only change field *values* within the fixed-size chunks
// above, never their size), and finally the `data` chunk holding `pcmData` verbatim (a trailing
// zero pad byte is added if its length is odd, per the RIFF spec's word-alignment rule).
// pcmData is assumed to already be correctly-encoded 16-bit interleaved samples -- this function
// does no audio processing of its own; see plugin/SampleImport.cpp for where those bytes come
// from (JUCE's own WAV writer, which reliably does the float->int16 conversion but doesn't know
// about this Roland-specific chunk). Returns an empty vector if numChannels/sampleRate aren't
// positive.
std::vector<std::byte> encodeWavWithRlndChunk(std::span<const std::byte> pcmData, int numChannels,
                                               uint32_t sampleRate, const RlndChunk& rlnd);

} // namespace sp404
