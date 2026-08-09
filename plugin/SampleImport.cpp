#include "SampleImport.h"

#include <cmath>

#include "sp404/SdCard.h"

namespace sp404 {

std::optional<std::vector<std::byte>> encodeToWav(const juce::AudioBuffer<float>& buffer) {
    const int numChannels = buffer.getNumChannels();
    const int numSamples = buffer.getNumSamples();
    if (numChannels <= 0 || numSamples <= 0)
        return std::nullopt;

    // Writes to a scratch temp file rather than an in-memory stream: WavAudioFormatWriter needs
    // to seek back and patch chunk-size fields on close, which is the well-trodden path via
    // FileOutputStream -- juce::MemoryOutputStream ownership gets transferred into (and destroyed
    // with) the writer, so there's no safe way to read the bytes back out of it afterwards.
    const auto tempFile = juce::File::getSpecialLocation(juce::File::tempDirectory)
                               .getChildFile("sp404_import_" +
                                             juce::String::toHexString(juce::Random::getSystemRandom().nextInt64()) +
                                             ".wav");

    {
        std::unique_ptr<juce::OutputStream> outStream = tempFile.createOutputStream();
        if (outStream == nullptr)
            return std::nullopt;

        juce::WavAudioFormat wavFormat;
        std::unique_ptr<juce::AudioFormatWriter> writer(wavFormat.createWriterFor(
            outStream, juce::AudioFormatWriterOptions{}
                           .withSampleRate(static_cast<double>(kNativeSampleRateHz))
                           .withNumChannels(numChannels)
                           .withBitsPerSample(16)));
        if (writer == nullptr)
            return std::nullopt;

        if (!writer->writeFromAudioSampleBuffer(buffer, 0, numSamples))
            return std::nullopt;
    } // writer destroyed here -- finalizes/patches the WAV header on disk

    juce::MemoryBlock block;
    const bool loaded = tempFile.loadFileAsData(block);
    tempFile.deleteFile();
    if (!loaded)
        return std::nullopt;

    const auto* raw = static_cast<const std::byte*>(block.getData());
    return std::vector<std::byte>(raw, raw + block.getSize());
}

namespace {

// Reads the whole reader into memory and resamples to kNativeSampleRateHz if needed (one
// juce::LagrangeInterpolator per channel -- each instance is stateful, sharing one across
// channels would corrupt the interpolation).
std::optional<std::vector<std::byte>> resampleAndEncode(juce::AudioFormatReader& reader) {
    const int numChannels = static_cast<int>(reader.numChannels);
    const int numInputSamples = static_cast<int>(reader.lengthInSamples);
    if (numChannels <= 0 || numInputSamples <= 0)
        return std::nullopt;

    juce::AudioBuffer<float> inputBuffer(numChannels, numInputSamples);
    reader.read(&inputBuffer, 0, numInputSamples, 0, true, true);

    juce::AudioBuffer<float> resampledBuffer;
    const juce::AudioBuffer<float>* bufferToWrite = &inputBuffer;

    const double sourceRate = reader.sampleRate;
    if (sourceRate > 0.0 && std::abs(sourceRate - static_cast<double>(kNativeSampleRateHz)) > 0.5) {
        const double speedRatio = sourceRate / static_cast<double>(kNativeSampleRateHz);
        const int numOutputSamples = static_cast<int>(std::ceil(static_cast<double>(numInputSamples) / speedRatio));
        resampledBuffer.setSize(numChannels, numOutputSamples);

        for (int channel = 0; channel < numChannels; ++channel) {
            juce::LagrangeInterpolator interpolator;
            interpolator.process(speedRatio, inputBuffer.getReadPointer(channel),
                                  resampledBuffer.getWritePointer(channel), numOutputSamples);
        }
        bufferToWrite = &resampledBuffer;
    }

    return encodeToWav(*bufferToWrite);
}

} // namespace

std::optional<std::vector<std::byte>> importAudioToWav(std::unique_ptr<juce::InputStream> input) {
    juce::AudioFormatManager formatManager;
    formatManager.registerBasicFormats();

    std::unique_ptr<juce::AudioFormatReader> reader(formatManager.createReaderFor(std::move(input)));
    if (reader == nullptr)
        return std::nullopt;

    return resampleAndEncode(*reader);
}

std::optional<std::vector<std::byte>> importAudioToWav(const juce::File& file) {
    juce::AudioFormatManager formatManager;
    formatManager.registerBasicFormats();

    std::unique_ptr<juce::AudioFormatReader> reader(formatManager.createReaderFor(file));
    if (reader == nullptr)
        return std::nullopt;

    return resampleAndEncode(*reader);
}

} // namespace sp404
