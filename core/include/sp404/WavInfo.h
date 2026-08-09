#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>

namespace sp404 {

// Just enough of a WAV file's RIFF/fmt/data chunk headers to report basic properties, without
// reading the audio payload itself. Little-endian, per the standard WAV/RIFF spec (unlike the
// Roland-specific big-endian PAD_INFO.BIN -- see docs/sp404sx-format.md).
struct WavInfo {
    uint32_t sampleRate = 0;
    uint16_t channels = 0;
    uint16_t bitsPerSample = 0;
    uint32_t dataSize = 0;   // bytes of audio data in the "data" chunk
    uint32_t dataOffset = 0; // byte offset of the "data" chunk's payload within the file

    double durationSeconds() const;
};

// Returns std::nullopt if wavPath isn't a readable RIFF/WAVE file, or is missing a "fmt "/"data"
// chunk.
std::optional<WavInfo> readWavInfo(const std::filesystem::path& wavPath);

} // namespace sp404
