#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_dsp/juce_dsp.h>

#include "BankLoader.h"

namespace sp404 {

// MIDI note 36 (C1 in the Ableton/FL Studio convention) triggers pad 1 of the active bank, up
// to note 47 (B1) = pad 12. Easy to change, but changing it changes what already-recorded MIDI
// tracks play.
constexpr int kBasePadNote = 36;
constexpr int kPadsPerBank = 12;

// At most this many distinct pads can sound at once; triggering another one steals (fades out)
// the oldest still-sounding pad. Retriggering a pad that's already sounding doesn't count as a
// new voice (it just restarts that same pad).
constexpr int kMaxPolyphony = 2;

// Volume / Low / Mid / High -- a master gain + 3-band shelf/peak EQ applied to the plugin's
// output mix (see PluginProcessor::updateOutputEq()), each a real 0-127 AudioProcessorParameter
// that can be dragged in the UI or MIDI-learned to a CC. Knob value 64 is the neutral/flat
// position for all four (unity gain / 0dB).
constexpr int kNumKnobs = 4;

class PluginProcessor final : public juce::AudioProcessor {
public:
    PluginProcessor();
    ~PluginProcessor() override = default;

    void prepareToPlay(double sampleRate, int samplesPerBlock) override;
    void releaseResources() override;
    bool isBusesLayoutSupported(const BusesLayout& layouts) const override;
    void processBlock(juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return JucePlugin_Name; }
    bool acceptsMidi() const override { return true; }
    bool producesMidi() const override { return false; }
    // One-shot pads can keep sounding after the triggering note is released (no envelope tail
    // in the reverb sense, just "the sample is still playing") -- a conservative fixed estimate
    // rather than 0, so hosts don't cut playback short at the end of a render/export.
    double getTailLengthSeconds() const override { return 4.0; }

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram(int) override {}
    const juce::String getProgramName(int) override { return {}; }
    void changeProgramName(int, const juce::String&) override {}

    void getStateInformation(juce::MemoryBlock&) override {}
    void setStateInformation(const void*, int) override {}

    // Called from WebUIBridge (message thread, on a UI bank click) or internally from
    // processBlock (audio thread, on a Program Change message) -- realtime-safe either way, see
    // BankLoader::requestBank().
    void requestBankChange(char bankName) { bankLoader.requestBank(bankName); }

    // Called from WebUIBridge (message thread, on a pad mousedown/mouseup in the UI) to preview
    // a pad of the active bank, padIndex 0-11. Safe to call from any thread: goes through
    // juce::MidiKeyboardState, which queues the event and merges it into the real MIDI stream on
    // the audio thread in processBlock -- the same trigger path as an actual incoming MIDI note,
    // so it correctly respects gate/loop/reverse.
    void previewPadOn(int padIndex) {
        if (padIndex >= 0 && padIndex < kPadsPerBank)
            keyboardState.noteOn(1, kBasePadNote + padIndex, 1.0f);
    }
    void previewPadOff(int padIndex) {
        if (padIndex >= 0 && padIndex < kPadsPerBank)
            keyboardState.noteOff(1, kBasePadNote + padIndex, 0.0f);
    }

    // Realtime-safe (single atomic load): bit i is set if pad i of the active bank is currently
    // sounding (including mid-fade-out). Polled from WebUIBridge's "getActivePads" so the UI can
    // show which pads are playing right now.
    std::uint16_t getActivePadMask() const { return activePadMask.load(std::memory_order_relaxed); }

    // Called from WebUIBridge (message thread, on the UI's Cancel/Stop button) to silence every
    // currently-sounding pad. Realtime-safe (single atomic store); the audio thread picks it up
    // at the top of the next processBlock and fades out (see stopVoice) whatever is active.
    void requestStopAll() { stopAllRequested.store(true, std::memory_order_relaxed); }

    // Realtime-safe (single atomic load): true if the output has clipped in roughly the last
    // 500ms (held rather than instantaneous, so a single-block transient is still visible when
    // polled at ~100ms intervals). Polled from WebUIBridge's "getClipping" to flash the UI's
    // screen graphic red.
    bool isClipping() const { return clippingFlag.load(std::memory_order_relaxed); }

    // --- Knobs (Vol/Ctrl1/Ctrl2/Ctrl3) -------------------------------------------------------
    // setKnobValue is called from the message thread (mouse drag in the UI) and goes through
    // the parameter's normal setValueNotifyingHost path, so host automation/undo see it like any
    // other parameter change. startKnobLearn/isKnobLearning/getKnobCc/getKnobValue are
    // realtime-safe atomic reads/writes, polled from and called by WebUIBridge.
    void setKnobValue(int knobIndex, int value0to127) {
        if (knobIndex < 0 || knobIndex >= kNumKnobs)
            return;
        *knobParams[static_cast<size_t>(knobIndex)] = juce::jlimit(0, 127, value0to127);
    }
    int getKnobValue(int knobIndex) const {
        return (knobIndex >= 0 && knobIndex < kNumKnobs) ? knobParams[static_cast<size_t>(knobIndex)]->get() : 0;
    }
    // Arms `knobIndex` to capture the next incoming MIDI CC number (see handleMidiMessage).
    void startKnobLearn(int knobIndex) {
        if (knobIndex >= 0 && knobIndex < kNumKnobs)
            learningKnobIndex.store(knobIndex, std::memory_order_relaxed);
    }
    bool isKnobLearning(int knobIndex) const {
        return learningKnobIndex.load(std::memory_order_relaxed) == knobIndex;
    }
    // -1 if `knobIndex` isn't MIDI-mapped yet.
    int getKnobCc(int knobIndex) const {
        return (knobIndex >= 0 && knobIndex < kNumKnobs) ? knobCcNumbers[static_cast<size_t>(knobIndex)].load(std::memory_order_relaxed) : -1;
    }

    // --- Offline/live sync mode ---------------------------------------------------------------
    // Live (default): every card access resolves the actually-connected SD card
    // (sp404::findConnectedCardRoot()). Offline: every card access resolves a local mirror
    // directory instead, so edits never touch the real card until an explicit "Synchroniser"
    // (see sp404::syncCard). resolveCardRoot() is the single choke point both WebUIBridge's
    // handlers and BankLoader (via the resolver callback passed to its constructor) go through --
    // nothing else in the codebase should call findConnectedCardRoot() directly once this mode
    // exists, so live vs offline can never be decided inconsistently between two call sites.
    //
    // The mirror lives at a fixed location (see mirrorRoot()) rather than one per physical card:
    // simplest model, matches "one working copy you sync when ready" rather than needing a picker
    // just to start offline editing.
    std::optional<std::filesystem::path> resolveCardRoot() const;
    static std::filesystem::path mirrorRoot();
    bool isOfflineMode() const { return offlineMode.load(std::memory_order_relaxed); }
    void setOfflineMode(bool offline) { offlineMode.store(offline, std::memory_order_relaxed); }

    // Small JSON prefs file for UI preferences that aren't tied to any one DAW project (currently
    // just the chosen theme, see WebUIBridge's getTheme/setTheme) -- same
    // getSpecialLocation(userApplicationDataDirectory) convention as mirrorRoot(), sibling file
    // rather than sibling directory since this is a single small file, not a mirrored folder tree.
    static std::filesystem::path prefsPath();

private:
    struct Voice {
        std::shared_ptr<const LoadedBank> bank; // captured at trigger time, see BankLoader.h
        int padIndex = -1;
        int rangeStart = 0;
        int rangeEnd = 0;
        double position = 0.0;
        bool active = false;
        int stopFadeRemaining = 0;
        int triggerOrder = 0; // for oldest-first voice stealing, see handleMidiMessage
    };

    void handleMidiMessage(const juce::MidiMessage& message);
    void renderVoices(juce::AudioBuffer<float>& buffer, int startSample, int numSamples);
    void stopVoice(Voice& voice);
    // Recomputes the low/mid/high filter coefficients from the current knob values. Called once
    // per processBlock (block-rate, not sample-rate -- coefficient calculation is cheap and this
    // keeps the EQ responsive to knob/MIDI-CC moves without needing a smoothed-parameter
    // ramping scheme) and once from prepareToPlay so the very first block is already correct.
    void updateOutputEq();

    BankLoader bankLoader;
    std::array<Voice, kPadsPerBank> voices;
    juce::MidiKeyboardState keyboardState;
    int nextTriggerOrder = 0;
    std::atomic<std::uint16_t> activePadMask{0};
    std::atomic<bool> stopAllRequested{false};
    std::atomic<bool> clippingFlag{false};
    double currentSampleRate = 44100.0;
    int clipHoldSamplesRemaining = 0;

    std::array<juce::AudioParameterInt*, kNumKnobs> knobParams{};
    std::array<std::atomic<int>, kNumKnobs> knobCcNumbers{};
    std::atomic<int> learningKnobIndex{-1}; // -1 = not currently MIDI-learning any knob

    // Master output EQ (post-mix, applied to every pad's summed signal right before it leaves
    // the plugin) -- one shelf/peak filter per band, duplicated across output channels by
    // ProcessorDuplicator. Order: low shelf, mid peak, high shelf.
    using ShelfFilter = juce::dsp::ProcessorDuplicator<juce::dsp::IIR::Filter<float>, juce::dsp::IIR::Coefficients<float>>;
    juce::dsp::ProcessorChain<ShelfFilter, ShelfFilter, ShelfFilter> outputEq;

    // Defaults to offline (mirror) rather than live: safer first-run behaviour (no accidental
    // writes to a connected card) and matches bankLoader's default of Bank A -- "just open and
    // play/edit the mirror" without needing a card plugged in at all. isOfflineMode()/
    // resolveCardRoot() already fall back to "no card" if the mirror isn't seeded yet, so this
    // is safe even on a machine that's never used offline mode before.
    std::atomic<bool> offlineMode{true};

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(PluginProcessor)
};

} // namespace sp404
