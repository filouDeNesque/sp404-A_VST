#pragma once

#include <cstddef>
#include <cstdint>

namespace sp404 {

// Roland PADINFO.BIN record layout (one per pad, big-endian on disk).
// See docs/sp404sx-format.md for the full byte-offset table and sources.
struct PadInfo {
    enum class Format : uint8_t { Aiff = 0, Wave = 1 };
    enum class TempoMode : uint8_t { Off = 0, Pattern = 1, User = 2 };

    static constexpr size_t encodedSize = 32;

    uint32_t origSampleStart = 0;
    uint32_t origSampleEnd = 0;
    uint32_t userSampleStart = 0;
    uint32_t userSampleEnd = 0;
    uint8_t volume = 0;
    bool lofi = false;
    bool loop = false;
    bool gate = false;
    bool reverse = false;
    Format format = Format::Wave;
    uint8_t channels = 1;
    TempoMode tempoMode = TempoMode::Off;
    uint32_t origTempo = 0; // BPM * 10
    uint32_t userTempo = 0; // BPM * 10

    // data must point at exactly encodedSize bytes.
    static PadInfo decode(const std::byte* data, size_t size);

    // out must point at exactly encodedSize bytes.
    void encode(std::byte* out, size_t size) const;
};

} // namespace sp404
