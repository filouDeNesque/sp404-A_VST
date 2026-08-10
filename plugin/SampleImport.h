#pragma once

#include <cstdint>
#include <juce_audio_formats/juce_audio_formats.h>

#include <optional>
#include <vector>

namespace sp404 {

// Decodes any audio file JUCE can read (WAV/AIFF/FLAC natively, plus MP3/MP4/M4A/AAC via
// CoreAudioFormat on macOS -- all already registered by AudioFormatManager::registerBasicFormats,
// no extra JUCE modules/macros needed, see plugin/CMakeLists.txt), resamples it to
// sp404::kNativeSampleRateHz if its own rate differs, and re-encodes the result as a 16-bit PCM
// WAV file with an embedded Roland "RLND" chunk (see sp404::encodeWavWithRlndChunk,
// core/include/sp404/WavRlnd.h) -- ready to hand to sp404::replacePadSample(), which just writes
// bytes verbatim and never itself checks/converts sample rate or bit depth (see
// core/include/sp404/SdCard.h). `sampleIndex` (see sp404::padSampleIndex) is the *destination*
// pad -- without a real RLND chunk carrying it, a real SP-404SX/A refuses to recognize the file
// as a valid pad sample at all (confirmed against a real card, see docs/sp404sx-format.md).
// Returns std::nullopt if the input can't be decoded (unrecognized/corrupt format).
std::optional<std::vector<std::byte>> importAudioToWav(std::unique_ptr<juce::InputStream> input,
                                                         std::uint8_t sampleIndex);
std::optional<std::vector<std::byte>> importAudioToWav(const juce::File& file, std::uint8_t sampleIndex);

// Encodes an in-memory buffer as a 16-bit PCM WAV file at sp404::kNativeSampleRateHz with an
// embedded RLND chunk for `sampleIndex` (see importAudioToWav's doc comment above), returning the
// raw bytes -- the tail half of importAudioToWav's pipeline, exposed on its own for
// plugin/SampleDsp.h, which processes a pad's *existing* sample (already at the native rate, no
// decode/resample step needed) and needs to re-encode the result the same way, for the same pad
// it read from. Returns std::nullopt if the buffer is empty or the WAV writer can't be created.
std::optional<std::vector<std::byte>> encodeToWav(const juce::AudioBuffer<float>& buffer, std::uint8_t sampleIndex);

} // namespace sp404
