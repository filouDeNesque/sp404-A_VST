#include "SampleDsp.h"

#include <juce_audio_formats/juce_audio_formats.h>

#include <rubberband/RubberBandStretcher.h>

#include <algorithm>
#include <cmath>
#include <numeric>
#include <vector>

#include "SampleImport.h"
#include "sp404/SdCard.h"

namespace sp404 {

namespace {

std::optional<std::filesystem::path> findPadSamplePath(const std::filesystem::path& sdRoot, char bank,
                                                         int indexInBank) {
    if (indexInBank < 1 || indexInBank > Bank::padCount)
        return std::nullopt;
    try {
        const auto card = SdCard::load(sdRoot);
        for (const auto& b : card.banks()) {
            if (b.name != bank)
                continue;
            return b.pads[static_cast<size_t>(indexInBank - 1)].samplePath;
        }
    } catch (const std::exception&) {
    }
    return std::nullopt;
}

std::optional<juce::AudioBuffer<float>> loadPadBuffer(const std::filesystem::path& sdRoot, char bank,
                                                        int indexInBank) {
    const auto samplePath = findPadSamplePath(sdRoot, bank, indexInBank);
    if (!samplePath)
        return std::nullopt;

    juce::AudioFormatManager formatManager;
    formatManager.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> reader(formatManager.createReaderFor(juce::File(samplePath->string())));
    if (reader == nullptr)
        return std::nullopt;

    const int numChannels = static_cast<int>(reader->numChannels);
    const int numSamples = static_cast<int>(reader->lengthInSamples);
    if (numChannels <= 0 || numSamples <= 0)
        return std::nullopt;

    juce::AudioBuffer<float> buffer(numChannels, numSamples);
    reader->read(&buffer, 0, numSamples, 0, true, true);
    return buffer;
}

bool writePadBuffer(const std::filesystem::path& sdRoot, char bank, int indexInBank,
                     const juce::AudioBuffer<float>& buffer) {
    const auto wavBytes = encodeToWav(buffer, static_cast<std::uint8_t>(padSampleIndex(bank, indexInBank)));
    if (!wavBytes)
        return false;
    try {
        replacePadSample(sdRoot, bank, indexInBank, *wavBytes);
        return true;
    } catch (const std::exception&) {
        return false;
    }
}

// Shared by trimSilencePad and getPadDspStatus: index (inclusive) of the first and last samples
// whose magnitude exceeds thresholdLinear, i.e. what trimming would cut down to. firstNonSilent >
// lastNonSilent means the whole buffer is at/under the threshold.
struct SilenceEdges {
    int firstNonSilent = 0;
    int lastNonSilent = -1;
};
SilenceEdges findSilenceEdges(const juce::AudioBuffer<float>& buffer, float thresholdLinear) {
    const int numSamples = buffer.getNumSamples();

    int firstNonSilent = 0;
    while (firstNonSilent < numSamples && buffer.getMagnitude(firstNonSilent, 1) <= thresholdLinear)
        ++firstNonSilent;

    int lastNonSilent = numSamples - 1;
    while (lastNonSilent > firstNonSilent && buffer.getMagnitude(lastNonSilent, 1) <= thresholdLinear)
        --lastNonSilent;

    return {firstNonSilent, lastNonSilent};
}

} // namespace

std::optional<PadDspStatus> getPadDspStatus(const std::filesystem::path& sdRoot, char bank, int indexInBank,
                                             float silenceThresholdLinear) {
    auto buffer = loadPadBuffer(sdRoot, bank, indexInBank);
    if (!buffer)
        return std::nullopt;

    PadDspStatus status;
    status.channels = buffer->getNumChannels();

    const int numSamples = buffer->getNumSamples();
    const float peak = buffer->getMagnitude(0, numSamples);
    status.isSilent = peak <= 0.0f;
    if (!status.isSilent)
        status.peakDb = juce::Decibels::gainToDecibels(peak);

    if (!status.isSilent) {
        const auto edges = findSilenceEdges(*buffer, silenceThresholdLinear);
        if (edges.firstNonSilent <= edges.lastNonSilent) {
            status.leadingSilenceSeconds = static_cast<float>(edges.firstNonSilent) / kNativeSampleRateHz;
            status.trailingSilenceSeconds =
                static_cast<float>(numSamples - 1 - edges.lastNonSilent) / kNativeSampleRateHz;
        }
    }

    return status;
}

bool normalizePad(const std::filesystem::path& sdRoot, char bank, int indexInBank, float targetPeakDb) {
    auto buffer = loadPadBuffer(sdRoot, bank, indexInBank);
    if (!buffer)
        return false;

    const float peak = buffer->getMagnitude(0, buffer->getNumSamples());
    if (peak <= 0.0f)
        return false; // silence -- nothing to normalize

    const float targetPeakLinear = juce::Decibels::decibelsToGain(targetPeakDb);
    buffer->applyGain(targetPeakLinear / peak);

    return writePadBuffer(sdRoot, bank, indexInBank, *buffer);
}

bool convertPadChannels(const std::filesystem::path& sdRoot, char bank, int indexInBank, int targetChannels) {
    if (targetChannels != 1 && targetChannels != 2)
        return false;

    auto buffer = loadPadBuffer(sdRoot, bank, indexInBank);
    if (!buffer)
        return false;

    const int sourceChannels = buffer->getNumChannels();
    if (sourceChannels == targetChannels)
        return writePadBuffer(sdRoot, bank, indexInBank, *buffer); // already there, still succeeds

    const int numSamples = buffer->getNumSamples();
    juce::AudioBuffer<float> converted(targetChannels, numSamples);

    if (sourceChannels == 2 && targetChannels == 1) {
        converted.copyFrom(0, 0, *buffer, 0, 0, numSamples);
        converted.addFrom(0, 0, *buffer, 1, 0, numSamples);
        converted.applyGain(0.5f);
    } else if (sourceChannels == 1 && targetChannels == 2) {
        converted.copyFrom(0, 0, *buffer, 0, 0, numSamples);
        converted.copyFrom(1, 0, *buffer, 0, 0, numSamples);
    } else {
        return false; // e.g. >2 source channels -- not expected for real SP-404SX pad content
    }

    return writePadBuffer(sdRoot, bank, indexInBank, converted);
}

bool fadePad(const std::filesystem::path& sdRoot, char bank, int indexInBank, double fadeInSeconds,
             double fadeOutSeconds) {
    auto buffer = loadPadBuffer(sdRoot, bank, indexInBank);
    if (!buffer)
        return false;

    const int numSamples = buffer->getNumSamples();
    const int fadeInSamples = juce::jlimit(0, numSamples, static_cast<int>(fadeInSeconds * kNativeSampleRateHz));
    const int fadeOutSamples = juce::jlimit(0, numSamples, static_cast<int>(fadeOutSeconds * kNativeSampleRateHz));

    if (fadeInSamples > 0)
        buffer->applyGainRamp(0, fadeInSamples, 0.0f, 1.0f);
    if (fadeOutSamples > 0)
        buffer->applyGainRamp(numSamples - fadeOutSamples, fadeOutSamples, 1.0f, 0.0f);

    return writePadBuffer(sdRoot, bank, indexInBank, *buffer);
}

bool trimSilencePad(const std::filesystem::path& sdRoot, char bank, int indexInBank, float thresholdLinear) {
    auto buffer = loadPadBuffer(sdRoot, bank, indexInBank);
    if (!buffer)
        return false;

    const int numSamples = buffer->getNumSamples();
    const int numChannels = buffer->getNumChannels();

    const auto edges = findSilenceEdges(*buffer, thresholdLinear);
    const int firstNonSilent = edges.firstNonSilent;
    const int lastNonSilent = edges.lastNonSilent;

    if (firstNonSilent >= lastNonSilent)
        return false; // entirely (or almost entirely) silent -- nothing sensible to trim to

    const int trimmedLength = lastNonSilent - firstNonSilent + 1;
    if (trimmedLength == numSamples)
        return writePadBuffer(sdRoot, bank, indexInBank, *buffer); // already tight, no-op

    juce::AudioBuffer<float> trimmed(numChannels, trimmedLength);
    for (int ch = 0; ch < numChannels; ++ch)
        trimmed.copyFrom(ch, 0, *buffer, ch, firstNonSilent, trimmedLength);

    return writePadBuffer(sdRoot, bank, indexInBank, trimmed);
}

std::optional<double> detectBpm(const std::filesystem::path& sdRoot, char bank, int indexInBank) {
    auto buffer = loadPadBuffer(sdRoot, bank, indexInBank);
    if (!buffer)
        return std::nullopt;

    const int numSamples = buffer->getNumSamples();
    const int numChannels = buffer->getNumChannels();
    if (numSamples < kNativeSampleRateHz / 2) // shorter than 0.5s -- not enough to estimate
        return std::nullopt;

    // Downmix to mono.
    std::vector<float> mono(static_cast<size_t>(numSamples), 0.0f);
    for (int ch = 0; ch < numChannels; ++ch) {
        const float* data = buffer->getReadPointer(ch);
        for (int i = 0; i < numSamples; ++i)
            mono[static_cast<size_t>(i)] += data[i];
    }
    const float downmixScale = 1.0f / static_cast<float>(numChannels);
    for (auto& s : mono)
        s *= downmixScale;

    // Energy envelope in ~10ms windows -- coarse enough to be cheap, fine enough to resolve the
    // 60-200 BPM lag range with reasonable precision.
    const int windowSize = kNativeSampleRateHz / 100;
    const int numWindows = numSamples / windowSize;
    if (numWindows < 20)
        return std::nullopt;

    std::vector<double> envelope(static_cast<size_t>(numWindows), 0.0);
    for (int w = 0; w < numWindows; ++w) {
        double sumSquares = 0.0;
        for (int i = 0; i < windowSize; ++i) {
            const float s = mono[static_cast<size_t>(w * windowSize + i)];
            sumSquares += static_cast<double>(s) * s;
        }
        envelope[static_cast<size_t>(w)] = std::sqrt(sumSquares / windowSize);
    }

    // Remove the mean so autocorrelation reflects periodicity, not overall level.
    const double mean = std::accumulate(envelope.begin(), envelope.end(), 0.0) / static_cast<double>(envelope.size());
    for (auto& e : envelope)
        e -= mean;

    constexpr double kWindowRateHz = 100.0; // 1 window = 10ms
    const int minLag = static_cast<int>(kWindowRateHz * 60.0 / 200.0); // 200 BPM
    const int maxLag = static_cast<int>(kWindowRateHz * 60.0 / 60.0);  // 60 BPM
    if (maxLag >= numWindows)
        return std::nullopt; // too short to see a full period even at the slowest plausible tempo

    int bestLag = -1;
    double bestScore = 0.0;
    for (int lag = minLag; lag <= maxLag; ++lag) {
        double score = 0.0;
        for (int i = 0; i + lag < numWindows; ++i)
            score += envelope[static_cast<size_t>(i)] * envelope[static_cast<size_t>(i + lag)];
        if (score > bestScore) {
            bestScore = score;
            bestLag = lag;
        }
    }

    if (bestLag <= 0 || bestScore <= 0.0)
        return std::nullopt;

    return 60.0 * kWindowRateHz / bestLag;
}

bool applyPitchTimeStretch(const std::filesystem::path& sdRoot, char bank, int indexInBank, double timeRatio,
                            double pitchSemitones) {
    if (std::abs(timeRatio - 1.0) < 1e-9 && std::abs(pitchSemitones) < 1e-9)
        return true; // no-op, don't touch the pad

    auto buffer = loadPadBuffer(sdRoot, bank, indexInBank);
    if (!buffer)
        return false;

    const int numChannels = buffer->getNumChannels();
    const int numSamples = buffer->getNumSamples();
    const double pitchScale = std::pow(2.0, pitchSemitones / 12.0);

    // Offline mode + R3 ("Finer") engine: highest quality, appropriate for a one-shot batch job
    // rather than real-time playback. The single-file build (RubberBandSingle.cpp) is compiled
    // with NO_THREADING, so process() below runs fully synchronously -- by the time it returns,
    // every output sample is already computed and buffered internally, ready to retrieve.
    RubberBand::RubberBandStretcher stretcher(
        static_cast<size_t>(kNativeSampleRateHz), static_cast<size_t>(numChannels),
        RubberBand::RubberBandStretcher::OptionProcessOffline | RubberBand::RubberBandStretcher::OptionEngineFiner,
        timeRatio, pitchScale);

    const float* const* inputPtrs = buffer->getArrayOfReadPointers();
    stretcher.study(inputPtrs, static_cast<size_t>(numSamples), true);
    stretcher.process(inputPtrs, static_cast<size_t>(numSamples), true);

    std::vector<std::vector<float>> outputChannels(static_cast<size_t>(numChannels));

    constexpr int kChunkSize = 4096;
    std::vector<std::vector<float>> chunkStorage(static_cast<size_t>(numChannels), std::vector<float>(kChunkSize));
    std::vector<float*> chunkPtrs(static_cast<size_t>(numChannels));
    for (int ch = 0; ch < numChannels; ++ch)
        chunkPtrs[static_cast<size_t>(ch)] = chunkStorage[static_cast<size_t>(ch)].data();

    while (true) {
        const int available = stretcher.available();
        if (available <= 0) // -1 = finished; 0 can't legitimately gain more data later (see above)
            break;

        const auto toRetrieve = static_cast<size_t>(std::min(available, kChunkSize));
        const size_t got = stretcher.retrieve(chunkPtrs.data(), toRetrieve);
        for (int ch = 0; ch < numChannels; ++ch) {
            auto& out = outputChannels[static_cast<size_t>(ch)];
            const auto& chunk = chunkStorage[static_cast<size_t>(ch)];
            out.insert(out.end(), chunk.begin(), chunk.begin() + static_cast<long>(got));
        }
    }

    const auto outputLength = static_cast<int>(outputChannels.empty() ? 0 : outputChannels[0].size());
    if (outputLength <= 0)
        return false;

    juce::AudioBuffer<float> result(numChannels, outputLength);
    for (int ch = 0; ch < numChannels; ++ch)
        result.copyFrom(ch, 0, outputChannels[static_cast<size_t>(ch)].data(), outputLength);

    return writePadBuffer(sdRoot, bank, indexInBank, result);
}

} // namespace sp404
