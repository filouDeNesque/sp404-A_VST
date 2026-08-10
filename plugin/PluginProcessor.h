#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <vector>

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_dsp/juce_dsp.h>

#include "BankLoader.h"
#include "sp404/PatternPlayer.h"
#include "sp404/SdCard.h"

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

    // Persists the active bank, offline/live mode, and the 4 knobs' values/CC mappings into the
    // DAW project (as XML, the standard JUCE convention for this -- see
    // AudioProcessor::copyXmlToBinary/getXmlFromBinary). Deliberately not everything the plugin
    // has: card contents/patterns already live on disk (the card itself or the offline mirror),
    // not in DAW project state, matching how e.g. a hardware sampler's own patches aren't part of
    // a DAW project either -- only *this instance's* transient UI/mapping state is.
    void getStateInformation(juce::MemoryBlock& destData) override;
    void setStateInformation(const void* data, int sizeInBytes) override;

    // Called from WebUIBridge (message thread, on a UI bank click) or internally from
    // processBlock (audio thread, on a Program Change message) -- realtime-safe either way, see
    // BankLoader::requestBank(). Also records the requested letter (independent of whether
    // BankLoader's background load has actually finished) so getStateInformation saves the
    // user's actual selection, not whatever happens to be loaded at save time.
    void requestBankChange(char bankName) {
        bankLoader.requestBank(bankName);
        lastRequestedBank.store(bankName, std::memory_order_relaxed);
    }

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

    // --- Pattern playback (tempo-synced to host BPM + gated on host transport play/stop; NOT
    // full bar-aligned host-transport-*position* sync -- see README roadmap for what that would
    // still take) -----------------------------------------------------------------------------
    // Message-thread only (does file I/O: reads the pattern file plus every bank its events
    // reference, since a pattern's storage slot and the banks it plays are independent of each
    // other -- see sp404::patternSlotPath's doc comment). Hands the result to the audio thread
    // the same way BankLoader hands off its currentBank (see BankLoader.h): a SpinLock-guarded
    // shared_ptr swap, picked up at the top of the next processBlock. Loops until stopPattern()
    // is called. Returns false if there's no pattern recorded at that slot.
    bool triggerPattern(char bank, int indexInBank);
    // Realtime-safe (single atomic store); the audio thread stops scheduling new triggers at the
    // top of the next processBlock. Doesn't forcibly silence pads already sounding from the
    // pattern -- use requestStopAll() for that (same as it already does for live/preview pads).
    void stopPattern() { patternStopRequested.store(true, std::memory_order_relaxed); }
    bool isPatternPlaying() const { return patternPlayingFlag.load(std::memory_order_relaxed); }
    // Which slot triggerPattern() was last called with -- meaningless if isPatternPlaying() is
    // false. Realtime-safe atomic reads, polled from WebUIBridge so the UI can highlight it.
    char getPlayingPatternBank() const { return static_cast<char>(playingPatternBank.load(std::memory_order_relaxed)); }
    int getPlayingPatternIndexInBank() const {
        return playingPatternIndexInBank.load(std::memory_order_relaxed);
    }

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
    // Sets a knob's CC mapping directly (ccOrMinusOne == -1 means "unmapped"), bypassing the
    // learn flow -- used by setStateInformation() to restore a mapping saved with the DAW
    // project. Realtime-safe (single atomic store), same as the rest of this section.
    void setKnobCc(int knobIndex, int ccOrMinusOne) {
        if (knobIndex >= 0 && knobIndex < kNumKnobs)
            knobCcNumbers[static_cast<size_t>(knobIndex)].store(ccOrMinusOne, std::memory_order_relaxed);
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
    // voiceSet is a parameter (rather than hardcoded to `voices`) so both live/preview playback
    // and pattern-triggered playback (see patternVoices below) can share the same mixing loop --
    // pure parameterization, the per-sample logic itself is unchanged from before pattern
    // playback existed.
    void renderVoices(juce::AudioBuffer<float>& buffer, int startSample, int numSamples, std::span<Voice> voiceSet);
    void stopVoice(Voice& voice);
    // Starts (or retriggers) a voice in voiceSet for bank/padIndex (0-based), stealing the oldest
    // active voice in voiceSet if it's already at maxPolyphony -- same voice-stealing rule
    // handleMidiMessage's note-on branch already uses for live MIDI, but generalized to an
    // arbitrary voice set/bank/polyphony budget rather than hardcoded to `voices`/the currently
    // armed bank/kMaxPolyphony. Needed because a pattern's events can reference a *different*
    // bank than whatever's currently armed for live MIDI (see docs/sp404sx-format.md), so pattern
    // playback needs its own voice pool (patternVoices) that can hold voices from any bank at
    // once rather than colliding with (or being limited to) live playback's single-armed-bank
    // pool. Deliberately not used by handleMidiMessage's existing note-on path -- that path's
    // direct `voices[padIndex]` indexing is untouched, to avoid any risk of changing already-
    // working live-triggering behaviour while adding this.
    void triggerVoice(std::span<Voice> voiceSet, int maxPolyphony, std::shared_ptr<const LoadedBank> bank,
                       int padIndex);
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
    std::atomic<char> lastRequestedBank{'A'}; // see requestBankChange()/getStateInformation()

    std::array<juce::AudioParameterInt*, kNumKnobs> knobParams{};
    std::array<std::atomic<int>, kNumKnobs> knobCcNumbers{};
    std::atomic<int> learningKnobIndex{-1}; // -1 = not currently MIDI-learning any knob

    // Master output EQ (post-mix, applied to every pad's summed signal right before it leaves
    // the plugin) -- one shelf/peak filter per band, duplicated across output channels by
    // ProcessorDuplicator. Order: low shelf, mid peak, high shelf.
    using ShelfFilter = juce::dsp::ProcessorDuplicator<juce::dsp::IIR::Filter<float>, juce::dsp::IIR::Coefficients<float>>;
    juce::dsp::ProcessorChain<ShelfFilter, ShelfFilter, ShelfFilter> outputEq;

    // --- Pattern playback ------------------------------------------------------------------
    static constexpr int kMaxPatternPolyphony = 4; // separate, modest budget from live playback's kMaxPolyphony
    std::array<Voice, kMaxPatternPolyphony> patternVoices;
    sp404::PatternPlayer patternPlayer;
    std::vector<sp404::PatternTriggerEvent> patternTriggerScratch; // reserved once in the constructor, cleared each block

    // Cross-thread handoff (message thread -> audio thread) of a newly-triggered pattern, exactly
    // mirroring BankLoader's own SpinLock+shared_ptr pattern for currentBank (see BankLoader.h) --
    // pragmatic, not wait-free, fine for a rare UI-triggered event rather than something touched
    // every block.
    juce::SpinLock patternHandoffLock;
    std::shared_ptr<const sp404::Pattern> pendingPattern; // set by triggerPattern(), consumed at the top of the next processBlock
    std::array<std::shared_ptr<const LoadedBank>, SdCard::numBanks> pendingPatternBanks; // parallel to pendingPattern, index = bank - 'A'
    std::array<std::shared_ptr<const LoadedBank>, SdCard::numBanks> activePatternBanks; // audio-thread-owned; only replaced when a new trigger is consumed
    std::atomic<bool> patternTriggerRequested{false};
    std::atomic<bool> patternStopRequested{false};
    std::atomic<bool> patternPlayingFlag{false};
    std::atomic<char> playingPatternBank{0};
    std::atomic<int> playingPatternIndexInBank{0};

    // Defaults to offline (mirror) rather than live: safer first-run behaviour (no accidental
    // writes to a connected card) and matches bankLoader's default of Bank A -- "just open and
    // play/edit the mirror" without needing a card plugged in at all. isOfflineMode()/
    // resolveCardRoot() already fall back to "no card" if the mirror isn't seeded yet, so this
    // is safe even on a machine that's never used offline mode before.
    std::atomic<bool> offlineMode{true};

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(PluginProcessor)
};

} // namespace sp404
