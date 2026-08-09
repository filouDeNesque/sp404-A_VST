#pragma once

#include <array>
#include <atomic>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>

#include <juce_audio_formats/juce_audio_formats.h>

#include "sp404/PadInfo.h"

namespace sp404 {

struct LoadedPad {
    juce::AudioBuffer<float> buffer;
    PadInfo info;
    bool hasSample = false;
};

struct LoadedBank {
    std::array<LoadedPad, 12> pads;
    char name = 'A';
};

// Reads every sample file for `bankName` on `cardRoot` into memory via `formatManager`. Pads
// without a readable sample file are left with hasSample = false. Returns nullptr if the bank
// itself can't be read (e.g. PAD_INFO.BIN missing) -- never throws.
//
// `shouldAbort` is checked before each per-pad file read and, if it returns true, loadBank bails
// out early and returns nullptr. BankLoader uses this so a slow SD card (e.g. a real card over a
// sluggish USB reader) can't make shutdown hang: without a cancellation point mid-load, a
// juce::Thread::stopThread() call racing a 12-file read can blow its timeout and force-kill the
// thread, which is unsafe. Defaults to "never abort" for direct/offline callers (tests, one-off
// verification tools) that don't need cancellation.
std::shared_ptr<const LoadedBank> loadBank(
    const std::filesystem::path& cardRoot, char bankName, juce::AudioFormatManager& formatManager,
    const std::function<bool()>& shouldAbort = [] { return false; });

// Owns background loading of banks off the audio thread.
//
// requestBank() is realtime-safe (a single atomic<char> store) and can be called from the audio
// thread (Program Change) or the message thread (UI click) alike -- all the actual file I/O
// happens on BankLoader's own juce::Thread, independent of whether a plugin editor is even open.
// It always re-reads from disk, even if the requested bank is already the current one: this is
// what makes a pad edit (see sp404::savePadInfo) show up in playback immediately after
// PluginProcessor::requestBankChange() re-arms the same bank.
//
// getCurrentBank() is realtime-safe: a SpinLock-protected shared_ptr copy, i.e. an atomic
// refcount increment with no allocation. This is a pragmatic cross-thread handoff (not
// wait-free), acceptable here because the critical section is a couple of instructions long.
class BankLoader : private juce::Thread {
public:
    // resolveCardRoot decides which card root to read from each cycle -- e.g. the real connected
    // card, or an offline mirror (see PluginProcessor::resolveCardRoot) -- so live/offline mode
    // is decided in exactly one place rather than BankLoader having its own opinion.
    explicit BankLoader(std::function<std::optional<std::filesystem::path>()> resolveCardRoot);
    ~BankLoader() override;

    void requestBank(char bankName);
    std::shared_ptr<const LoadedBank> getCurrentBank() const;

private:
    void run() override;

    std::function<std::optional<std::filesystem::path>()> resolveCardRoot;
    juce::AudioFormatManager formatManager;

    juce::SpinLock bankLock;
    std::shared_ptr<const LoadedBank> currentBank;

    std::atomic<char> requestedBankName{0};

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(BankLoader)
};

} // namespace sp404
