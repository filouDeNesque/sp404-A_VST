#include "BackgroundOperation.h"

namespace sp404 {

BackgroundOperation::~BackgroundOperation() {
    if (thread.joinable())
        thread.join();
}

bool BackgroundOperation::start(std::function<juce::var(const ProgressCallback&)> work) {
    if (running.load(std::memory_order_acquire))
        return false;

    // A previous operation's thread has already finished running (running == false) but its
    // std::thread object may not be joined yet -- must join before reassigning `thread` below,
    // or std::thread::operator= on a still-joinable thread calls std::terminate().
    if (thread.joinable())
        thread.join();

    running.store(true, std::memory_order_release);
    done.store(false, std::memory_order_release);
    current.store(0, std::memory_order_relaxed);
    total.store(0, std::memory_order_relaxed);
    {
        const juce::SpinLock::ScopedLockType lock(stateLock);
        label = juce::String();
        result = juce::var();
    }

    thread = std::thread([this, workToRun = std::move(work)]() mutable {
        const ProgressCallback onProgress = [this](int newCurrent, int newTotal, const std::string& newLabel) {
            current.store(newCurrent, std::memory_order_relaxed);
            total.store(newTotal, std::memory_order_relaxed);
            const juce::SpinLock::ScopedLockType lock(stateLock);
            label = juce::String(newLabel);
        };

        juce::var workResult = workToRun(onProgress);

        {
            const juce::SpinLock::ScopedLockType lock(stateLock);
            result = std::move(workResult);
        }
        // done before running: a poller that observes done==true is guaranteed to also see
        // running==false, never a stale "still running" read racing the very next start() call.
        done.store(true, std::memory_order_release);
        running.store(false, std::memory_order_release);
    });

    return true;
}

BackgroundOperation::Snapshot BackgroundOperation::poll() {
    Snapshot snapshot;
    snapshot.running = running.load(std::memory_order_acquire);
    snapshot.done = done.load(std::memory_order_acquire);
    snapshot.current = current.load(std::memory_order_relaxed);
    snapshot.total = total.load(std::memory_order_relaxed);
    {
        const juce::SpinLock::ScopedLockType lock(stateLock);
        snapshot.label = label;
        snapshot.result = result;
    }
    return snapshot;
}

} // namespace sp404
