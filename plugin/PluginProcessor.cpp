#include "PluginProcessor.h"

#include <algorithm>
#include <limits>
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
    patternTriggerScratch.reserve(32); // avoids a mid-playback allocation for all but pathologically dense patterns

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

void PluginProcessor::triggerVoice(std::span<Voice> voiceSet, int maxPolyphony, std::shared_ptr<const LoadedBank> bank,
                                    int padIndex, std::uint8_t velocity) {
    if (bank == nullptr || padIndex < 0 || padIndex >= kPadsPerBank)
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

    // Reuse an already-active voice for this exact (bank, pad) if one exists in this set --
    // retriggering doesn't count against maxPolyphony, it just restarts below. Otherwise steal
    // the oldest active voice in the set if already at maxPolyphony, then use any free slot.
    Voice* target = nullptr;
    for (auto& v : voiceSet) {
        if (v.active && v.padIndex == padIndex && v.bank.get() == bank.get()) {
            target = &v;
            break;
        }
    }

    if (target == nullptr) {
        int activeCount = 0;
        for (auto& v : voiceSet)
            if (v.active)
                ++activeCount;

        if (activeCount >= maxPolyphony) {
            Voice* oldest = nullptr;
            for (auto& v : voiceSet) {
                if (v.active && (oldest == nullptr || v.triggerOrder < oldest->triggerOrder))
                    oldest = &v;
            }
            if (oldest != nullptr)
                stopVoice(*oldest);
        }

        for (auto& v : voiceSet) {
            if (!v.active) {
                target = &v;
                break;
            }
        }
        // Every slot still active (mid-fade-out from the steal above) -- reuse the one just
        // stolen rather than dropping the trigger; cosmetic edge case, not expected in practice
        // since maxPolyphony-sized sets always have at least one slot free the instant a steal
        // happens above.
        if (target == nullptr)
            target = &voiceSet.front();
    }

    target->bank = std::move(bank);
    target->padIndex = padIndex;
    target->rangeStart = rangeStart;
    target->rangeEnd = rangeEnd;
    target->position = pad.info.reverse ? static_cast<double>(rangeEnd - 1) : static_cast<double>(rangeStart);
    target->stopFadeRemaining = 0;
    target->triggerOrder = nextTriggerOrder++;
    target->velocityGain = static_cast<float>(velocity) / 127.0f;
    target->active = rangeEnd > rangeStart;
}

bool PluginProcessor::triggerPattern(char bank, int indexInBank) {
    const auto cardRoot = resolveCardRoot();
    if (!cardRoot)
        return false;

    const auto pattern = readPattern(patternSlotPath(*cardRoot, bank, indexInBank));
    if (!pattern)
        return false;

    // Loads every bank this pattern's events actually reference -- usually just one (see
    // docs/sp404sx-format.md), but not necessarily `bank` itself, since a pattern's storage slot
    // and the banks its notes reference are independent of each other.
    std::array<std::shared_ptr<const LoadedBank>, SdCard::numBanks> loadedBanks{};
    juce::AudioFormatManager formatManager;
    formatManager.registerBasicFormats();
    for (const auto& event : pattern->events) {
        const auto padBank = event.bank();
        if (!padBank)
            continue;
        const auto idx = static_cast<size_t>(*padBank - 'A');
        if (loadedBanks[idx] == nullptr)
            loadedBanks[idx] = loadBank(*cardRoot, *padBank, formatManager);
    }

    auto patternPtr = std::make_shared<const Pattern>(*pattern);
    {
        const juce::SpinLock::ScopedLockType lock(patternHandoffLock);
        pendingPattern = std::move(patternPtr);
        pendingPatternBanks = loadedBanks;
    }
    // release: pairs with processBlock's acquire exchange, so it never sees the flag flip before
    // the pendingPattern/pendingPatternBanks writes above are visible to it.
    patternTriggerRequested.store(true, std::memory_order_release);

    // Not realtime-touched (message thread only) -- plain relaxed stores are fine, the audio
    // thread only reads these for UI polling, not to gate anything timing-sensitive.
    playingPatternBank.store(bank, std::memory_order_relaxed);
    playingPatternIndexInBank.store(indexInBank, std::memory_order_relaxed);
    return true;
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

        Voice& voice = voices[static_cast<size_t>(padIndex)];

        // A looping pad has no natural end (renderVoices wraps its position back to rangeStart
        // forever instead of ever setting voice.active = false) and, if it's also ungated, never
        // gets a note-off that would stop it either (see the note-off branch below) -- pressing it
        // again is the only way to stop it, so a re-press of an already-sounding loop pad toggles
        // it off instead of retriggering it from the start.
        if (pad.info.loop && voice.active && voice.padIndex == padIndex) {
            stopVoice(voice);
            return;
        }

        int rangeStart = static_cast<int>(pad.info.userSampleStart);
        int rangeEnd = static_cast<int>(pad.info.userSampleEnd);
        const int numSamples = pad.buffer.getNumSamples();
        if (rangeEnd <= rangeStart || rangeEnd > numSamples) {
            rangeStart = 0;
            rangeEnd = numSamples;
        }

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
        // Deliberately not derived from message.getVelocity() -- live MIDI doesn't model velocity
        // at all (see the Voice::velocityGain doc comment and README Limitations); every pad
        // always plays at its own configured volume regardless of what a controller sends.
        voice.velocityGain = 1.0f;
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

void PluginProcessor::renderVoices(juce::AudioBuffer<float>& buffer, int startSample, int numSamples,
                                    std::span<Voice> voiceSet) {
    if (numSamples <= 0)
        return;

    const int outChannels = buffer.getNumChannels();

    for (auto& voice : voiceSet) {
        if (!voice.active || voice.bank == nullptr)
            continue;

        const auto& pad = voice.bank->pads[static_cast<size_t>(voice.padIndex)];
        if (!pad.hasSample) {
            voice.active = false;
            continue;
        }

        const float gain = static_cast<float>(pad.info.volume) / 127.0f * voice.velocityGain;
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

    if (stopAllRequested.exchange(false, std::memory_order_relaxed)) {
        for (auto& voice : voices)
            stopVoice(voice);
        for (auto& voice : patternVoices)
            stopVoice(voice);
        patternPlayer.stop();
        patternPlayingFlag.store(false, std::memory_order_relaxed);
        pendingLaunchPattern = nullptr; // cancel a queued-to-launch trigger too, not just a playing one
    }

    if (patternStopRequested.exchange(false, std::memory_order_relaxed)) {
        patternPlayer.stop();
        patternPlayingFlag.store(false, std::memory_order_relaxed);
        pendingLaunchPattern = nullptr; // same: don't let an armed trigger surprise-launch after Stop
        // Unconditional (not gate-only, unlike the note-off handling below): a pad that's still
        // sounding because it's gated *and* looping would otherwise never get a note-off again
        // once the scheduler that would have sent one has stopped, and keep looping forever.
        for (auto& voice : patternVoices)
            stopVoice(voice);
    }

    // Queried once per block and reused below for both starting a newly-triggered pattern at the
    // right tempo and for keeping an already-playing one in sync -- this is *tempo* sync plus
    // *play/stop* sync, plus *launch*-quantization to the next bar boundary (see below) -- not
    // full continuous bar-aligned *position* sync to the host timeline (see the class's doc
    // comment in PluginProcessor.h for what that would still take). Hosts that don't report a
    // play state at all default to "playing", so pattern playback still works when hosted
    // standalone/by a simple test host with no transport concept. ppqPosition stays unset (and
    // launch-quantization falls back to starting immediately, see below) for the same reason.
    double hostBpm = 120.0;
    bool hostIsPlaying = true;
    juce::Optional<double> ppqPosition;
    int timeSigNumerator = 4;
    int timeSigDenominator = 4;
    if (auto* playHead = getPlayHead()) {
        if (const auto position = playHead->getPosition()) {
            hostBpm = position->getBpm().orFallback(120.0);
            hostIsPlaying = position->getIsPlaying();
            ppqPosition = position->getPpqPosition();
            if (const auto timeSig = position->getTimeSignature()) {
                timeSigNumerator = timeSig->numerator;
                timeSigDenominator = timeSig->denominator;
            }
        }
    }

    // acquire: pairs with triggerPattern()'s release store, so the pendingPattern/
    // pendingPatternBanks reads just below always see that call's writes, not a stale/partial
    // view of them.
    if (patternTriggerRequested.exchange(false, std::memory_order_acquire)) {
        std::shared_ptr<const Pattern> newPattern;
        {
            const juce::SpinLock::ScopedLockType lock(patternHandoffLock);
            newPattern = pendingPattern;
            activePatternBanks = pendingPatternBanks;
        }
        if (newPattern != nullptr) {
            if (ppqPosition) {
                // Arm rather than start immediately: PatternPlayer::start() actually runs once
                // the block below sees the host's PPQ position reach pendingLaunchPpq. Setting
                // patternPlayingFlag now (not only once it actually launches) is deliberate UI
                // feedback that the trigger was registered, even during the wait for the bar.
                pendingLaunchPattern = newPattern;
                pendingLaunchPpq = nextBarBoundaryPpq(*ppqPosition, timeSigNumerator, timeSigDenominator);
            } else {
                patternPlayer.start(*newPattern, hostBpm, currentSampleRate);
            }
            patternPlayingFlag.store(true, std::memory_order_relaxed);
        }
    } else {
        patternPlayer.setTempo(hostBpm);
    }

    // A pattern armed just above (or in an earlier block, if its bar boundary hasn't arrived yet)
    // actually launches once the host's PPQ position reaches it. No-op every block a trigger isn't
    // pending. Falls back to launching immediately if the host stops reporting a PPQ position
    // between arming and launch (shouldn't happen in practice, but avoids a trigger silently never
    // starting if it did).
    if (pendingLaunchPattern != nullptr) {
        if (!ppqPosition || *ppqPosition >= pendingLaunchPpq) {
            patternPlayer.start(*pendingLaunchPattern, hostBpm, currentSampleRate);
            pendingLaunchPattern = nullptr;
        }
    }

    // Merges any pending previewPadOn()/previewPadOff() calls (from a UI pad click) into `midi`
    // as real note on/off messages, so they go through the exact same handleMidiMessage() path
    // as actual MIDI input below.
    keyboardState.processNextMidiBuffer(midi, 0, buffer.getNumSamples(), true);

    patternTriggerScratch.clear();
    if (patternPlayer.isPlaying() && hostIsPlaying) {
        patternPlayer.advance(buffer.getNumSamples(), patternTriggerScratch);
        // advance() appends a block's note-offs after that block's note-ons, which for a very
        // short note can put a note-off earlier in sample-time behind a later note-on in vector
        // order -- sort so the merge below can walk both event sources with a simple two-pointer
        // pass (see PatternPlayer::advance's doc comment). Small vector (a handful of events in
        // the overwhelming majority of blocks), so this is not a meaningful audio-thread cost.
        std::sort(patternTriggerScratch.begin(), patternTriggerScratch.end(),
                  [](const PatternTriggerEvent& a, const PatternTriggerEvent& b) {
                      return a.sampleOffsetInBlock < b.sampleOffsetInBlock;
                  });
    }

    // Merges the real MIDI buffer (already sample-sorted, guaranteed by JUCE) with
    // patternTriggerScratch (sorted just above) via a manual two-pointer walk, so both event
    // sources render at their correct sample-accurate position within the block without needing
    // to allocate a combined list.
    int samplePos = 0;
    auto midiIt = midi.begin();
    const auto midiEnd = midi.end();
    size_t patternIdx = 0;

    while (midiIt != midiEnd || patternIdx < patternTriggerScratch.size()) {
        const int midiSample = midiIt != midiEnd ? (*midiIt).samplePosition : std::numeric_limits<int>::max();
        const int patternSample =
            patternIdx < patternTriggerScratch.size() ? patternTriggerScratch[patternIdx].sampleOffsetInBlock
                                                        : std::numeric_limits<int>::max();

        const int eventSample = std::min(midiSample, patternSample);
        if (eventSample > samplePos) {
            renderVoices(buffer, samplePos, eventSample - samplePos, voices);
            renderVoices(buffer, samplePos, eventSample - samplePos, patternVoices);
        }
        samplePos = eventSample;

        if (midiSample <= patternSample) {
            handleMidiMessage((*midiIt).getMessage());
            ++midiIt;
        } else {
            const auto& event = patternTriggerScratch[patternIdx];
            if (!event.isNoteOff) {
                const auto bankIdx = static_cast<size_t>(event.bank - 'A');
                if (bankIdx < activePatternBanks.size() && activePatternBanks[bankIdx] != nullptr)
                    triggerVoice(patternVoices, kMaxPatternPolyphony, activePatternBanks[bankIdx],
                                 event.padIndexInBank - 1, event.velocity);
            } else {
                // Same "only cut a *gated* pad early" rule as handleMidiMessage's live-MIDI
                // note-off below -- an ungated pad just keeps playing to its natural end/loop.
                const int padIndex = event.padIndexInBank - 1;
                for (auto& voice : patternVoices) {
                    if (voice.active && voice.padIndex == padIndex && voice.bank != nullptr &&
                        voice.bank->name == event.bank) {
                        if (voice.bank->pads[static_cast<size_t>(padIndex)].info.gate)
                            stopVoice(voice);
                        break;
                    }
                }
            }
            ++patternIdx;
        }
    }
    renderVoices(buffer, samplePos, buffer.getNumSamples() - samplePos, voices);
    renderVoices(buffer, samplePos, buffer.getNumSamples() - samplePos, patternVoices);

    // Master output stage: volume + 3-band EQ, applied to the summed mix of every pad rather
    // than per-voice -- this is deliberately post-mix (see the knob doc comment in
    // PluginProcessor.h), so it also colours the clip detection below.
    updateOutputEq();
    buffer.applyGain(juce::Decibels::decibelsToGain(knobToDb(getKnobValue(0), kVolumeMinDb, kVolumeMaxDb)));
    juce::dsp::AudioBlock<float> block(buffer);
    outputEq.process(juce::dsp::ProcessContextReplacing<float>(block));

    // Scoped to `voices` only (not patternVoices) -- this mask means "pads of the active bank
    // shown in the pad grid", and a pattern's voices can belong to a different bank than that,
    // see triggerPattern()'s doc comment. A pattern hit on a pad that *is* in the active bank
    // still won't light up here, since it plays through the separate patternVoices pool rather
    // than `voices` -- a minor, accepted gap (see README), not a correctness issue for playback.
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

void PluginProcessor::getStateInformation(juce::MemoryBlock& destData) {
    juce::XmlElement xml("SP404CompanionState");
    xml.setAttribute("formatVersion", 1);
    xml.setAttribute("activeBank",
                      juce::String::charToString(static_cast<juce::juce_wchar>(lastRequestedBank.load(std::memory_order_relaxed))));
    xml.setAttribute("offlineMode", isOfflineMode());

    for (int i = 0; i < kNumKnobs; ++i) {
        auto* knobXml = xml.createNewChildElement("Knob");
        knobXml->setAttribute("index", i);
        knobXml->setAttribute("value", getKnobValue(i));
        knobXml->setAttribute("cc", getKnobCc(i));
    }

    copyXmlToBinary(xml, destData);
}

void PluginProcessor::setStateInformation(const void* data, int sizeInBytes) {
    const std::unique_ptr<juce::XmlElement> xml(getXmlFromBinary(data, sizeInBytes));
    if (xml == nullptr || xml->getTagName() != "SP404CompanionState")
        return; // not our XML (e.g. a corrupt/foreign project file) -- leave current state as-is

    const auto bankStr = xml->getStringAttribute("activeBank", "A");
    if (bankStr.length() == 1) {
        const char bank = static_cast<char>(bankStr[0]);
        if (bank >= 'A' && bank <= 'J')
            requestBankChange(bank);
    }

    setOfflineMode(xml->getBoolAttribute("offlineMode", false));

    for (auto* knobXml : xml->getChildWithTagNameIterator("Knob")) {
        const int index = knobXml->getIntAttribute("index", -1);
        if (index < 0 || index >= kNumKnobs)
            continue;
        setKnobValue(index, knobXml->getIntAttribute("value", 64));
        setKnobCc(index, knobXml->getIntAttribute("cc", -1));
    }
}

juce::AudioProcessorEditor* PluginProcessor::createEditor() {
    return new PluginEditor(*this);
}

} // namespace sp404

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() {
    return new sp404::PluginProcessor();
}
