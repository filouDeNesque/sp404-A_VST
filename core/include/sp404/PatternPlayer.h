#pragma once

#include <cstdint>
#include <vector>

#include "sp404/Pattern.h"

namespace sp404 {

// One scheduled trigger, emitted by PatternPlayer::advance(): which pad to play, and how many
// samples into the block being processed it should fire. isNoteOff distinguishes a note landing
// (velocity carries through, see below) from that same note's scheduled release
// (tick + lengthTicks): the caller decides what a note-off actually does (this plugin's live MIDI
// only cuts a *gated* pad early on note-off, and lets an ungated one play out regardless -- see
// PluginProcessor::handleMidiMessage -- pattern playback follows the same rule).
struct PatternTriggerEvent {
    char bank = 'A';
    int padIndexInBank = 1;
    int sampleOffsetInBlock = 0; // 0-based, always < the numSamples passed to advance()
    uint8_t velocity = 0;       // meaningless (0) on a note-off; carried through from the pattern
                                  // event on a note-on. This plugin's live MIDI triggering already
                                  // ignores velocity -- pads always play at their own configured
                                  // volume, see docs/README -- so this exists for a future caller
                                  // that might want it, not because one uses it yet.
    bool isNoteOff = false;
};

// Tracks real-time playback of one Pattern, tempo-synced to a caller-supplied BPM (typically the
// DAW host's current tempo) -- NOT synced to the host's bar/beat *position*, i.e. this is "start
// now, advance at host tempo, loop" rather than a bar-aligned clip that always lands on the same
// point in the host's timeline. See PluginProcessor for how advancing is additionally gated on
// host transport play/stop.
//
// Pure C++, no JUCE dependency -- designed to be driven once per audio block (see advance()) by
// PluginProcessor::processBlock, but with no allocation or file I/O of its own, so it's fully
// unit-testable offline (see core/tests/PatternPlayerTests.cpp) without a real audio thread.
//
// NOT thread-safe by itself: start()/stop()/advance() must all be called from the same thread
// (the audio thread, in the real plugin -- see PluginProcessor's SpinLock-guarded handoff of a
// newly-triggered Pattern from the message thread, mirroring BankLoader's existing pattern for
// currentBank).
class PatternPlayer {
public:
    // Copies `pattern` in (small: at most a few hundred 8-byte events) and starts playback from
    // the beginning. No-op (playback stays stopped) if the pattern has no events or totalTicks()
    // <= 0. bpm/sampleRate must both be positive; see setTempo() to update bpm while playing
    // (e.g. the host's tempo changed).
    void start(const Pattern& pattern, double bpm, double sampleRate);
    void stop();
    bool isPlaying() const { return playing; }

    // Changes tempo with effect from the next advance() call. Commits the tick position reached
    // under the *old* tempo first (see .cpp) so the change doesn't retroactively reinterpret
    // samples already advanced through -- only future advance() calls run at the new rate.
    void setTempo(double newBpm);

    // Advances playback by numSamples (a no-op if not playing, or numSamples <= 0), appending
    // every event that fires during that span to outEvents (not cleared first -- caller's
    // responsibility, so multiple sources can share one scratch buffer across a block if wanted).
    // Loops back to the start of the pattern automatically -- see the class comment.
    //
    // Every note-on with a nonzero lengthTicks schedules a matching note-off (see
    // PatternTriggerEvent), which may fire many blocks later (or after several loops, for a note
    // that's still sustaining when playback wraps) -- tracked internally, no caller bookkeeping
    // needed. Within one advance() call, note-offs are appended to outEvents *after* that block's
    // note-ons, which can put a note-off earlier in tick-time behind a later-in-tick-time note-on
    // in vector order for very short notes -- outEvents is not guaranteed globally sorted by
    // sampleOffsetInBlock; sort it yourself first if your merge logic depends on that (see
    // PluginProcessor::processBlock, which does).
    void advance(int numSamples, std::vector<PatternTriggerEvent>& outEvents);

private:
    // A note-on scheduled to receive a matching note-off once playback reaches offAbsoluteTick --
    // may span many advance() calls (and even loop wraps) between being scheduled and firing.
    struct PendingNoteOff {
        double offAbsoluteTick = 0.0;
        char bank = 'A';
        int padIndexInBank = 1;
    };

    Pattern pattern;
    std::vector<int> eventTicks; // absoluteEventTicks(pattern), cached at start() so advance() never recomputes it
    std::vector<PendingNoteOff> pendingNoteOffs; // typically tiny (a handful at most); linear scan is fine
    double bpm = 120.0;
    double sampleRate = 44100.0;

    // Tick position is tracked as tickPositionAtEpoch + samplesSinceEpoch/samplesPerTick(bpm),
    // recomputed fresh from the exact integer samplesSinceEpoch on every advance() call, rather
    // than accumulated by repeatedly adding each block's already-rounded result to a running
    // double -- the latter drifts measurably over the many thousands of blocks a real playback
    // session involves (caught by a test that processes a full loop in ~700 odd-sized blocks and
    // found an extra spurious trigger from the accumulated error alone). A tempo change commits
    // the current position into tickPositionAtEpoch and resets samplesSinceEpoch to 0, so each
    // "epoch" between tempo changes gets its own single-division-per-call precision, and epoch
    // boundaries (rare -- only on setTempo(), not every block) are the only place any rounding
    // error can accumulate at all.
    double tickPositionAtEpoch = 0.0;
    int64_t samplesSinceEpoch = 0;

    int nextEventIndex = 0;
    int loopCount = 0;
    bool playing = false;

    double samplesPerTick() const;
};

// Computes the PPQ position of the next bar boundary at or after currentPpq, for the given host
// time signature (e.g. 4, 4). Used to "arm" a triggered pattern to launch on the host's next bar
// rather than starting immediately mid-bar -- see PluginProcessor::triggerPattern's doc comment
// for why this is *launch* quantization only, not continuous position sync. If currentPpq already
// sits on a bar boundary (within a small epsilon), returns currentPpq itself -- no need to wait a
// full bar when the trigger lands exactly on one. Falls back to assuming 4/4 if either time
// signature field is not positive (matches the fallback already used for a host that doesn't
// report a time signature at all).
double nextBarBoundaryPpq(double currentPpq, int timeSigNumerator, int timeSigDenominator);

} // namespace sp404
