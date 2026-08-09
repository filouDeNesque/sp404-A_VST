#include "PluginProcessor.h"

#include <algorithm>
#include <system_error>

#include "PluginEditor.h"
#include "sp404/SdCard.h"

namespace sp404 {

namespace {
constexpr int kStopFadeSamples = 64; // ~1.3ms at 48kHz: enough to avoid an audible click on stop

// Output EQ band centre frequencies/Q -- fixed (not user-adjustable), only each band's gain is
// under knob control. Roughly "bass/body/air" split for a sample-based drum machine companion.
constexpr float kLowShelfFreqHz = 200.0f;
constexpr float kMidPeakFreqHz = 1000.0f;
constexpr float kHighShelfFreqHz = 4000.0f;
constexpr float kShelfQ = 0.707f; // Butterworth Q, standard "no resonant bump" shelf
constexpr float kPeakQ = 1.0f;
constexpr float kEqRangeDb = 12.0f; // low/mid/high knobs sweep -12dB..+12dB around their centre
constexpr float kVolumeMinDb = -60.0f; // practically silent, not true -inf (avoids log(0))
constexpr float kVolumeMaxDb = 6.0f;

// Maps a 0-127 knob value to a dB amount either side of 0dB, with 64 landing exactly on 0dB
// (the neutral/flat position for every one of these four knobs) rather than an off-by-a-fraction
// value from a single linear jmap() across the whole 0-127 range.
float knobToDb(int value0to127, float dbAtMin, float dbAtMax) {
    const auto value = static_cast<float>(juce::jlimit(0, 127, value0to127));
    return value <= 64.0f ? juce::jmap(value, 0.0f, 64.0f, dbAtMin, 0.0f)
                           : juce::jmap(value, 64.0f, 127.0f, 0.0f, dbAtMax);
}
} // namespace

PluginProcessor::PluginProcessor()
    : AudioProcessor(BusesProperties().withOutput("Output", juce::AudioChannelSet::stereo(), true)),
      bankLoader([this] { return resolveCardRoot(); }) {
    bankLoader.requestBank('A');

    static const char* const knobNames[kNumKnobs] = {"Volume", "Low", "Mid", "High"};
    for (int i = 0; i < kNumKnobs; ++i) {
        knobCcNumbers[static_cast<size_t>(i)] = -1;
        auto* param = new juce::AudioParameterInt(juce::ParameterID("knob" + juce::String(i), 1), knobNames[i], 0,
                                                    127, 64);
        knobParams[static_cast<size_t>(i)] = param;
        addParameter(param);
    }
}

void PluginProcessor::prepareToPlay(double sampleRate, int samplesPerBlock) {
    currentSampleRate = sampleRate;

    juce::dsp::ProcessSpec spec;
    spec.sampleRate = sampleRate;
    spec.maximumBlockSize = static_cast<juce::uint32>(samplesPerBlock);
    spec.numChannels = static_cast<juce::uint32>(getTotalNumOutputChannels());
    outputEq.prepare(spec);
    updateOutputEq();
}

void PluginProcessor::updateOutputEq() {
    const double sr = currentSampleRate;
    const float lowDb = knobToDb(getKnobValue(1), -kEqRangeDb, kEqRangeDb);
    const float midDb = knobToDb(getKnobValue(2), -kEqRangeDb, kEqRangeDb);
    const float highDb = knobToDb(getKnobValue(3), -kEqRangeDb, kEqRangeDb);

    *outputEq.get<0>().state = *juce::dsp::IIR::Coefficients<float>::makeLowShelf(
        sr, kLowShelfFreqHz, kShelfQ, juce::Decibels::decibelsToGain(lowDb));
    *outputEq.get<1>().state = *juce::dsp::IIR::Coefficients<float>::makePeakFilter(
        sr, kMidPeakFreqHz, kPeakQ, juce::Decibels::decibelsToGain(midDb));
    *outputEq.get<2>().state = *juce::dsp::IIR::Coefficients<float>::makeHighShelf(
        sr, kHighShelfFreqHz, kShelfQ, juce::Decibels::decibelsToGain(highDb));
}

std::filesystem::path PluginProcessor::mirrorRoot() {
    return juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
        .getChildFile("SP404Companion")
        .getChildFile("OfflineMirror")
        .getFullPathName()
        .toStdString();
}

std::filesystem::path PluginProcessor::prefsPath() {
    return juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
        .getChildFile("SP404Companion")
        .getChildFile("Prefs.json")
        .getFullPathName()
        .toStdString();
}

std::optional<std::filesystem::path> PluginProcessor::resolveCardRoot() const {
    if (isOfflineMode()) {
        const auto mirror = mirrorRoot();
        std::error_code ec;
        const auto padInfoSize = std::filesystem::file_size(smplDir(mirror) / "PAD_INFO.BIN", ec);
        const auto expectedSize = static_cast<uintmax_t>(SdCard::totalPads) * PadInfo::encodedSize;
        if (ec || padInfoSize != expectedSize)
            return std::nullopt; // mirror not seeded yet -- see WebUIBridge's enterOfflineMode
        return mirror;
    }
    return findConnectedCardRoot();
}

void PluginProcessor::releaseResources() {}

bool PluginProcessor::isBusesLayoutSupported(const BusesLayout& layouts) const {
    return layouts.getMainOutputChannelSet() == juce::AudioChannelSet::stereo();
}

void PluginProcessor::stopVoice(Voice& voice) {
    if (voice.active && voice.stopFadeRemaining == 0)
        voice.stopFadeRemaining = kStopFadeSamples;
}

void PluginProcessor::handleMidiMessage(const juce::MidiMessage& message) {
    if (message.isNoteOn()) {
        const int padIndex = message.getNoteNumber() - kBasePadNote;
        if (padIndex < 0 || padIndex >= kPadsPerBank)
            return;

        auto bank = bankLoader.getCurrentBank();
        if (bank == nullptr)
            return;

        const auto& pad = bank->pads[static_cast<size_t>(padIndex)];
        if (!pad.hasSample)
            return;

        int rangeStart = static_cast<int>(pad.info.userSampleStart);
        int rangeEnd = static_cast<int>(pad.info.userSampleEnd);
        const int numSamples = pad.buffer.getNumSamples();
        if (rangeEnd <= rangeStart || rangeEnd > numSamples) {
            rangeStart = 0;
            rangeEnd = numSamples;
        }

        Voice& voice = voices[static_cast<size_t>(padIndex)];

        // Voice stealing: retriggering a pad that's already sounding doesn't add to the
        // concurrent-pad count (it just restarts below). Triggering a *different* pad while
        // kMaxPolyphony pads are already active cuts the oldest one first.
        if (!voice.active) {
            int activeCount = 0;
            for (const auto& v : voices)
                if (v.active)
                    ++activeCount;

            if (activeCount >= kMaxPolyphony) {
                Voice* oldest = nullptr;
                for (auto& v : voices) {
                    if (v.active && (oldest == nullptr || v.triggerOrder < oldest->triggerOrder))
                        oldest = &v;
                }
                if (oldest != nullptr)
                    stopVoice(*oldest);
            }
        }

        voice.bank = std::move(bank);
        voice.padIndex = padIndex;
        voice.rangeStart = rangeStart;
        voice.rangeEnd = rangeEnd;
        voice.position = pad.info.reverse ? static_cast<double>(rangeEnd - 1) : static_cast<double>(rangeStart);
        voice.stopFadeRemaining = 0;
        voice.triggerOrder = nextTriggerOrder++;
        voice.active = rangeEnd > rangeStart;
    } else if (message.isNoteOff()) {
        const int padIndex = message.getNoteNumber() - kBasePadNote;
        if (padIndex < 0 || padIndex >= kPadsPerBank)
            return;

        Voice& voice = voices[static_cast<size_t>(padIndex)];
        if (voice.active && voice.padIndex == padIndex && voice.bank != nullptr) {
            const auto& pad = voice.bank->pads[static_cast<size_t>(padIndex)];
            if (pad.info.gate)
                stopVoice(voice);
        }
    } else if (message.isProgramChange()) {
        const int program = message.getProgramChangeNumber();
        if (program >= 0 && program < SdCard::numBanks)
            requestBankChange(static_cast<char>('A' + program));
    } else if (message.isController()) {
        const int learning = learningKnobIndex.load(std::memory_order_relaxed);
        if (learning >= 0) {
            knobCcNumbers[static_cast<size_t>(learning)].store(message.getControllerNumber(),
                                                                 std::memory_order_relaxed);
            learningKnobIndex.store(-1, std::memory_order_relaxed);
        } else {
            for (int i = 0; i < kNumKnobs; ++i) {
                if (knobCcNumbers[static_cast<size_t>(i)].load(std::memory_order_relaxed) !=
                    message.getControllerNumber())
                    continue;

                // AudioParameterInt::setValue() is private (JUCE steers callers towards
                // setValueNotifyingHost()), so this does involve a host/listener notification
                // from the audio thread -- not textbook realtime-safe, but a widely-used
                // pattern for MIDI-CC-to-parameter mapping in JUCE plugins, and fine here since
                // nothing else is subscribed as a listener.
                knobParams[static_cast<size_t>(i)]->setValueNotifyingHost(
                    static_cast<float>(message.getControllerValue()) / 127.0f);
            }
        }
    }
}

void PluginProcessor::renderVoices(juce::AudioBuffer<float>& buffer, int startSample, int numSamples) {
    if (numSamples <= 0)
        return;

    const int outChannels = buffer.getNumChannels();

    for (auto& voice : voices) {
        if (!voice.active || voice.bank == nullptr)
            continue;

        const auto& pad = voice.bank->pads[static_cast<size_t>(voice.padIndex)];
        if (!pad.hasSample) {
            voice.active = false;
            continue;
        }

        const float gain = static_cast<float>(pad.info.volume) / 127.0f;
        const int srcChannels = pad.buffer.getNumChannels();
        const int direction = pad.info.reverse ? -1 : 1;

        // No sample-rate conversion in this MVP: playback advances one source sample per output
        // sample, so a pad recorded at a rate different from the host's session rate will play
        // back at a slightly wrong speed/pitch. Fine for the common 44.1kHz case (the SP-404SX's
        // own export format), a known limitation otherwise -- see README roadmap.
        for (int i = 0; i < numSamples; ++i) {
            if (!voice.active)
                break;

            const int pos = static_cast<int>(voice.position);
            if (pos < 0 || pos >= pad.buffer.getNumSamples()) {
                voice.active = false;
                break;
            }

            float fade = 1.0f;
            if (voice.stopFadeRemaining > 0) {
                fade = static_cast<float>(voice.stopFadeRemaining) / static_cast<float>(kStopFadeSamples);
                if (--voice.stopFadeRemaining == 0)
                    voice.active = false;
            }

            for (int ch = 0; ch < outChannels; ++ch) {
                const float sample = pad.buffer.getSample(std::min(ch, srcChannels - 1), pos);
                buffer.addSample(ch, startSample + i, sample * gain * fade);
            }

            voice.position += direction;

            const bool reachedEnd =
                pad.info.reverse ? (voice.position < voice.rangeStart) : (voice.position >= voice.rangeEnd);
            if (reachedEnd) {
                if (pad.info.loop)
                    voice.position = pad.info.reverse ? static_cast<double>(voice.rangeEnd - 1)
                                                       : static_cast<double>(voice.rangeStart);
                else
                    voice.active = false;
            }
        }
    }
}

void PluginProcessor::processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi) {
    juce::ScopedNoDenormals noDenormals;
    buffer.clear();

    if (stopAllRequested.exchange(false, std::memory_order_relaxed))
        for (auto& voice : voices)
            stopVoice(voice);

    // Merges any pending previewPadOn()/previewPadOff() calls (from a UI pad click) into `midi`
    // as real note on/off messages, so they go through the exact same handleMidiMessage() path
    // as actual MIDI input below.
    keyboardState.processNextMidiBuffer(midi, 0, buffer.getNumSamples(), true);

    int samplePos = 0;
    for (const auto metadata : midi) {
        const int eventSample = metadata.samplePosition;
        if (eventSample > samplePos)
            renderVoices(buffer, samplePos, eventSample - samplePos);
        samplePos = eventSample;
        handleMidiMessage(metadata.getMessage());
    }
    renderVoices(buffer, samplePos, buffer.getNumSamples() - samplePos);

    // Master output stage: volume + 3-band EQ, applied to the summed mix of every pad rather
    // than per-voice -- this is deliberately post-mix (see the knob doc comment in
    // PluginProcessor.h), so it also colours the clip detection below.
    updateOutputEq();
    buffer.applyGain(juce::Decibels::decibelsToGain(knobToDb(getKnobValue(0), kVolumeMinDb, kVolumeMaxDb)));
    juce::dsp::AudioBlock<float> block(buffer);
    outputEq.process(juce::dsp::ProcessContextReplacing<float>(block));

    std::uint16_t mask = 0;
    for (size_t i = 0; i < voices.size(); ++i)
        if (voices[i].active)
            mask = static_cast<std::uint16_t>(mask | (1u << i));
    activePadMask.store(mask, std::memory_order_relaxed);

    float peak = 0.0f;
    for (int ch = 0; ch < buffer.getNumChannels(); ++ch) {
        const auto* data = buffer.getReadPointer(ch);
        for (int i = 0; i < buffer.getNumSamples(); ++i)
            peak = std::max(peak, std::abs(data[i]));
    }
    constexpr float kClipThreshold = 0.98f;
    if (peak >= kClipThreshold)
        clipHoldSamplesRemaining = static_cast<int>(currentSampleRate * 0.5); // ~500ms hold
    else
        clipHoldSamplesRemaining = std::max(0, clipHoldSamplesRemaining - buffer.getNumSamples());
    clippingFlag.store(clipHoldSamplesRemaining > 0, std::memory_order_relaxed);
}

juce::AudioProcessorEditor* PluginProcessor::createEditor() {
    return new PluginEditor(*this);
}

} // namespace sp404

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() {
    return new sp404::PluginProcessor();
}
