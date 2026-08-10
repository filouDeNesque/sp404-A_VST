#pragma once

#include <juce_core/juce_core.h>

#include <atomic>
#include <functional>
#include <thread>

#include "sp404/Progress.h"

namespace sp404 {

// Runs one save/load/sync operation (see BankArchive.h/PatternArchive.h/SdCard.h's
// ProgressCallback-taking functions) on a background thread, republishing its progress for the
// message thread to poll -- so a multi-hundred-file zip/sync doesn't block the WebView (and the
// whole plugin UI with it) for however long that takes, and so the UI can show real step-by-step
// progress instead of a single "please wait" with no idea how far along it is.
//
// One operation at a time per instance (matches how the UI actually drives this: one menu action
// clicked at a time) -- start() returns false without doing anything if one is already running.
// Not copyable/movable (owns a std::thread and shared state a running thread points back into).
class BackgroundOperation {
public:
    BackgroundOperation() = default;
    // Joins any still-running thread -- this blocks until whatever file operation is in flight
    // finishes, rather than risk tearing it down mid-write (a half-written zip/half-synced card
    // is worse than a plugin that takes an extra moment to close).
    ~BackgroundOperation();

    BackgroundOperation(const BackgroundOperation&) = delete;
    BackgroundOperation& operator=(const BackgroundOperation&) = delete;

    // Starts `work` on a background thread, passing it a ProgressCallback it should forward into
    // whichever sp404:: function it calls. `work` returns a juce::var describing the outcome
    // (whatever shape the caller wants -- e.g. {ok, exportedCount}), published once `work`
    // returns and readable via poll() from then on. Returns false (doesn't start anything) if an
    // operation is already running.
    bool start(std::function<juce::var(const ProgressCallback&)> work);

    struct Snapshot {
        bool running = false;
        bool done = false; // true once, and remains true until the next start() call
        int current = 0;
        int total = 0;
        juce::String label;
        juce::var result; // meaningful only once done == true
    };

    // Safe to call from the message thread at any time, including while nothing has ever been
    // started (returns a default-constructed Snapshot, running == done == false).
    Snapshot poll();

private:
    std::thread thread;
    std::atomic<bool> running{false};
    std::atomic<bool> done{false};
    std::atomic<int> current{0};
    std::atomic<int> total{0};

    // Guards label/result -- both written once from the background thread (label repeatedly
    // while it runs, result once at the very end) and read from the message thread via poll().
    juce::SpinLock stateLock;
    juce::String label;
    juce::var result;
};

} // namespace sp404
