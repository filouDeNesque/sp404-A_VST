#include "sp404/PatternPlayer.h"

#include <algorithm>
#include <cmath>

namespace sp404 {

double PatternPlayer::samplesPerTick() const {
    // kTicksPerBar covers 4 beats (4/4 assumed, matching every real pattern verified so far --
    // see docs/sp404sx-format.md), so kTicksPerBar/4 = ticks/beat.
    constexpr double ticksPerBeat = kTicksPerBar / 4.0;
    return (60.0 * sampleRate) / (bpm * ticksPerBeat);
}

void PatternPlayer::start(const Pattern& newPattern, double newBpm, double newSampleRate) {
    pattern = newPattern;
    eventTicks = absoluteEventTicks(pattern);
    bpm = newBpm > 0.0 ? newBpm : bpm;
    sampleRate = newSampleRate > 0.0 ? newSampleRate : sampleRate;
    tickPositionAtEpoch = 0.0;
    samplesSinceEpoch = 0;
    nextEventIndex = 0;
    loopCount = 0;
    playing = !pattern.events.empty() && pattern.totalTicks() > 0;
}

void PatternPlayer::stop() {
    playing = false;
}

void PatternPlayer::setTempo(double newBpm) {
    if (!(newBpm > 0.0))
        return;
    if (playing) {
        const double spt = samplesPerTick();
        if (spt > 0.0)
            tickPositionAtEpoch += static_cast<double>(samplesSinceEpoch) / spt;
        samplesSinceEpoch = 0;
    }
    bpm = newBpm;
}

void PatternPlayer::advance(int numSamples, std::vector<PatternTriggerEvent>& outEvents) {
    if (!playing || numSamples <= 0)
        return;

    const double spt = samplesPerTick();
    if (!(spt > 0.0)) // guards NaN/inf from a pathological bpm/sampleRate too, not just <= 0
        return;

    const double blockStartTick = tickPositionAtEpoch + static_cast<double>(samplesSinceEpoch) / spt;
    samplesSinceEpoch += numSamples;
    const double blockEndTick = tickPositionAtEpoch + static_cast<double>(samplesSinceEpoch) / spt;

    const int totalTicks = pattern.totalTicks();

    // Walk forward through events (in order, looping via loopCount) until the next one's
    // absolute tick position -- loopCount * totalTicks + its own tick within the pattern -- falls
    // at or after this block's end. Each event at or after blockStartTick but before blockEndTick
    // fires in this block, at the sample offset its exact tick position maps to.
    while (true) {
        const double eventAbsoluteTick =
            static_cast<double>(loopCount) * totalTicks + eventTicks[static_cast<size_t>(nextEventIndex)];
        if (eventAbsoluteTick >= blockEndTick)
            break;

        const double samplesIntoBlock = (eventAbsoluteTick - blockStartTick) * spt;
        const int offset =
            std::clamp(static_cast<int>(std::llround(samplesIntoBlock)), 0, std::max(0, numSamples - 1));

        const auto& event = pattern.events[static_cast<size_t>(nextEventIndex)];
        const auto bank = event.bank();
        const auto padIndex = event.padIndexInBank();
        if (bank && padIndex)
            outEvents.push_back({*bank, *padIndex, offset, event.velocity});

        ++nextEventIndex;
        if (nextEventIndex >= static_cast<int>(pattern.events.size())) {
            nextEventIndex = 0;
            ++loopCount;
        }
    }
}

} // namespace sp404
