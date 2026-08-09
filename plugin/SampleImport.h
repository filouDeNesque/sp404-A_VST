#pragma once

#include <juce_audio_formats/juce_audio_formats.h>

#include <optional>
#include <vector>

namespace sp404 {

// Decodes any audio file JUCE can read (WAV/AIFF/FLAC natively, plus MP3/MP4/M4A/AAC via
// CoreAudioFormat on macOS -- all already registered by AudioFormatManager::registerBasicFormats,
// no extra JUCE modules/macros needed, see plugin/CMakeLists.txt), resamples it to
// sp404::kNativeSampleRateHz if its own rate differs, and re-encodes the result as a 16-bit PCM
// WAV file -- ready to hand to sp404::replacePadSample(), which just writes bytes verbatim and
// never itself checks/converts sample rate or bit depth (see core/include/sp404/SdCard.h).
// Returns std::nullopt if the input can't be decoded (unrecognized/corrupt format).
std::optional<std::vector<std::byte>> importAudioToWav(std::unique_ptr<juce::InputStream> input);
std::optional<std::vector<std::byte>> importAudioToWav(const juce::File& file);

// Encodes an in-memory buffer as a 16-bit PCM WAV file at sp404::kNativeSampleRateHz, returning
// the raw bytes -- the tail half of importAudioToWav's pipeline, exposed on its own for
// plugin/SampleDsp.h, which processes a pad's *existing* sample (already at the native rate, no
// decode/resample step needed) and needs to re-encode the result the same way. Returns
// std::nullopt if the buffer is empty or the WAV writer can't be created.
std::optional<std::vector<std::byte>> encodeToWav(const juce::AudioBuffer<float>& buffer);

} // namespace sp404
