#pragma once

#include <filesystem>
#include <optional>

namespace sp404 {

// Each of these reads a pad's *existing* sample from the card, applies one transformation, and
// writes the result back via sp404::replacePadSample -- so, exactly like importing a brand new
// file onto the pad, the trim (userSampleStart/End) resets to cover the whole processed file;
// playback settings (volume/loop/gate/reverse/lofi) are preserved (replacePadSample's existing
// contract, unchanged). All return false if the pad has no sample, or the result couldn't be
// re-encoded/written.

// Read-only snapshot of a pad's *existing* sample, for the DSP panel to show "what's true right
// now" (peak level, channel count, edge silence) before/after an action -- so the UI can say
// "already normalized"/"already stereo"/"no silence to trim" instead of a generic "done" that
// looks identical whether anything actually changed.
struct PadDspStatus {
    int channels = 0;
    bool isSilent = true;
    float peakDb = -100.0f;               // meaningless if isSilent
    float leadingSilenceSeconds = 0.0f;    // measured with the same threshold as trimSilencePad
    float trailingSilenceSeconds = 0.0f;
};
std::optional<PadDspStatus> getPadDspStatus(const std::filesystem::path& sdRoot, char bank, int indexInBank,
                                             float silenceThresholdLinear = 0.01f);

// Peak-normalizes to targetPeakDb (default -1 dBFS, leaving a little headroom). No-op (still
// returns true) if the sample is silent -- nothing to scale.
bool normalizePad(const std::filesystem::path& sdRoot, char bank, int indexInBank, float targetPeakDb = -1.0f);

// targetChannels must be 1 (mono -- averages existing channels) or 2 (stereo -- duplicates a
// mono source into both channels). No-op if already at targetChannels.
bool convertPadChannels(const std::filesystem::path& sdRoot, char bank, int indexInBank, int targetChannels);

bool fadePad(const std::filesystem::path& sdRoot, char bank, int indexInBank, double fadeInSeconds,
             double fadeOutSeconds);

// Strips leading/trailing silence (all channels' magnitude <= thresholdLinear) from the sample.
// Returns false (no write) if the whole sample is silent.
bool trimSilencePad(const std::filesystem::path& sdRoot, char bank, int indexInBank, float thresholdLinear = 0.01f);

// Read-only -- doesn't touch the pad. Estimates BPM via autocorrelation of the sample's energy
// envelope (~10ms windows, searched over the 60-200 BPM lag range). Intended for short loops/
// one-shots (typical SP-404 pad content), not full songs -- see plugin/SampleDsp.cpp for why a
// simple autocorrelation approach is a reasonable fit here despite being far simpler than a real
// beat-tracker. Returns std::nullopt if the pad has no sample, the sample is too short to
// analyze (< 0.5s, or shorter than one period at 60 BPM), or no clear periodicity is found.
std::optional<double> detectBpm(const std::filesystem::path& sdRoot, char bank, int indexInBank);

// Pitch-shifts (semitones, +/-) and/or time-stretches (ratio: >1 = longer/slower, <1 = shorter/
// faster) via Rubber Band Library (offline mode, R3/"Finer" engine). timeRatio==1.0 &&
// pitchSemitones==0.0 is a no-op (returns true without touching the pad).
bool applyPitchTimeStretch(const std::filesystem::path& sdRoot, char bank, int indexInBank, double timeRatio,
                            double pitchSemitones);

} // namespace sp404
