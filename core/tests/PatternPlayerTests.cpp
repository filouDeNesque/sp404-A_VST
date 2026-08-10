#include <cmath>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "sp404/PatternPlayer.h"

namespace {

// 4 quarter-note-spaced events (one per beat of a 4/4 bar), on 4 distinct pads so each fired
// event is unambiguous. bars=1 -> totalTicks() == 384.
sp404::Pattern makeQuarterNotePattern() {
    sp404::Pattern pattern;
    pattern.events.push_back(sp404::makeNoteEvent('A', 1, 0, 100, 10));  // tick 0
    pattern.events.push_back(sp404::makeNoteEvent('B', 2, 96, 110, 10)); // tick 96
    pattern.events.push_back(sp404::makeNoteEvent('C', 3, 96, 120, 10)); // tick 192
    pattern.events.push_back(sp404::makeNoteEvent('D', 4, 96, 130, 10)); // tick 288
    pattern.bars = 1;
    return pattern;
}

} // namespace

TEST_CASE("PatternPlayer fires every event at its exact sample offset within one block", "[PatternPlayer]") {
    sp404::PatternPlayer player;
    // bpm=120, sampleRate=48000 -> samplesPerTick = 60*48000/(120*96) = 250 exactly.
    player.start(makeQuarterNotePattern(), 120.0, 48000.0);
    REQUIRE(player.isPlaying());

    std::vector<sp404::PatternTriggerEvent> events;
    player.advance(384 * 250, events); // exactly one full bar

    REQUIRE(events.size() == 4);
    CHECK(events[0].bank == 'A');
    CHECK(events[0].padIndexInBank == 1);
    CHECK(events[0].sampleOffsetInBlock == 0);
    CHECK(events[1].bank == 'B');
    CHECK(events[1].sampleOffsetInBlock == 24000);
    CHECK(events[2].bank == 'C');
    CHECK(events[2].sampleOffsetInBlock == 48000);
    CHECK(events[3].bank == 'D');
    CHECK(events[3].sampleOffsetInBlock == 72000);
}

TEST_CASE("PatternPlayer loops back to the start and keeps firing on the next advance() call", "[PatternPlayer]") {
    sp404::PatternPlayer player;
    player.start(makeQuarterNotePattern(), 120.0, 48000.0);

    std::vector<sp404::PatternTriggerEvent> events;
    player.advance(384 * 250, events); // consume exactly one full bar
    REQUIRE(events.size() == 4);
    events.clear();

    player.advance(48000, events); // half a bar into the second loop (2 beats = 192 ticks)
    REQUIRE(events.size() == 2);
    CHECK(events[0].bank == 'A');
    CHECK(events[0].sampleOffsetInBlock == 0);
    CHECK(events[1].bank == 'B');
    CHECK(events[1].sampleOffsetInBlock == 24000);
}

TEST_CASE("PatternPlayer doesn't fire an event that lands exactly on the next block's start", "[PatternPlayer]") {
    // Advancing exactly one full bar should fire loop 0's 4 events and *not* loop 1's event 0,
    // which sits exactly at the boundary (see the advance()'s `>=` comparison).
    sp404::PatternPlayer player;
    player.start(makeQuarterNotePattern(), 120.0, 48000.0);

    std::vector<sp404::PatternTriggerEvent> events;
    player.advance(384 * 250, events);
    CHECK(events.size() == 4);

    std::vector<sp404::PatternTriggerEvent> nextBlockEvents;
    player.advance(1, nextBlockEvents); // the very next sample should immediately fire loop 1's event 0
    REQUIRE(nextBlockEvents.size() == 1);
    CHECK(nextBlockEvents[0].bank == 'A');
    CHECK(nextBlockEvents[0].sampleOffsetInBlock == 0);
}

TEST_CASE("PatternPlayer stays drift-free across many odd-sized blocks over a full loop", "[PatternPlayer]") {
    sp404::PatternPlayer player;
    player.start(makeQuarterNotePattern(), 120.0, 48000.0);

    // 96000 samples (one full bar at 250 samples/tick) in blocks of 137 -- deliberately not a
    // divisor of 250 or 96000, to stress the fractional tickPosition accumulation.
    constexpr int kTotalSamples = 384 * 250;
    constexpr int kBlockSize = 137;

    std::vector<int> absoluteSamplePositions;
    std::vector<char> banksFired;
    int samplesDone = 0;
    while (samplesDone < kTotalSamples) {
        const int thisBlock = std::min(kBlockSize, kTotalSamples - samplesDone);
        std::vector<sp404::PatternTriggerEvent> events;
        player.advance(thisBlock, events);
        for (const auto& e : events) {
            absoluteSamplePositions.push_back(samplesDone + e.sampleOffsetInBlock);
            banksFired.push_back(e.bank);
        }
        samplesDone += thisBlock;
    }

    REQUIRE(absoluteSamplePositions.size() == 4);
    CHECK(banksFired == std::vector<char>{'A', 'B', 'C', 'D'});
    // +/-1 sample tolerance for llround() landing on either side of a block boundary.
    CHECK(std::abs(absoluteSamplePositions[0] - 0) <= 1);
    CHECK(std::abs(absoluteSamplePositions[1] - 24000) <= 1);
    CHECK(std::abs(absoluteSamplePositions[2] - 48000) <= 1);
    CHECK(std::abs(absoluteSamplePositions[3] - 72000) <= 1);
}

TEST_CASE("PatternPlayer stays drift-free over many loops at a typical DAW block size", "[PatternPlayer]") {
    sp404::PatternPlayer player;
    player.start(makeQuarterNotePattern(), 120.0, 48000.0); // 250 samples/tick, 96000 samples/loop

    constexpr int kBlockSize = 512; // a common real-world host buffer size, not a divisor of 96000
    constexpr int kLoops = 200;
    constexpr int kTotalSamples = 384 * 250 * kLoops;

    int firedCount = 0;
    char lastBank = 0;
    int samplesDone = 0;
    while (samplesDone < kTotalSamples) {
        const int thisBlock = std::min(kBlockSize, kTotalSamples - samplesDone);
        std::vector<sp404::PatternTriggerEvent> events;
        player.advance(thisBlock, events);
        for (const auto& e : events) {
            ++firedCount;
            lastBank = e.bank;
        }
        samplesDone += thisBlock;
    }

    // Exactly 4 events/loop, no extra/missing firings from accumulated floating-point error over
    // ~188 blocks/loop * 200 loops (~37600 advance() calls).
    CHECK(firedCount == 4 * kLoops);
    CHECK(lastBank == 'D'); // the 200th loop's last event, same as every loop's last event
}

TEST_CASE("PatternPlayer stop()/isPlaying()", "[PatternPlayer]") {
    sp404::PatternPlayer player;
    CHECK_FALSE(player.isPlaying());

    player.start(makeQuarterNotePattern(), 120.0, 48000.0);
    CHECK(player.isPlaying());

    player.stop();
    CHECK_FALSE(player.isPlaying());

    std::vector<sp404::PatternTriggerEvent> events;
    player.advance(100000, events); // no-op once stopped
    CHECK(events.empty());
}

TEST_CASE("PatternPlayer refuses to start on an empty pattern or zero-bar pattern", "[PatternPlayer]") {
    sp404::PatternPlayer player;

    sp404::Pattern empty;
    empty.bars = 4; // has bars but no events
    player.start(empty, 120.0, 48000.0);
    CHECK_FALSE(player.isPlaying());

    sp404::Pattern zeroBars;
    zeroBars.events.push_back(sp404::makeNoteEvent('A', 1, 0, 100, 10));
    zeroBars.bars = 0; // has an event but totalTicks() == 0
    player.start(zeroBars, 120.0, 48000.0);
    CHECK_FALSE(player.isPlaying());
}

TEST_CASE("PatternPlayer::advance is a no-op for zero/negative numSamples", "[PatternPlayer]") {
    sp404::PatternPlayer player;
    player.start(makeQuarterNotePattern(), 120.0, 48000.0);

    std::vector<sp404::PatternTriggerEvent> events;
    player.advance(0, events);
    player.advance(-100, events);
    CHECK(events.empty());
    CHECK(player.isPlaying()); // still playing, just hasn't moved
}

TEST_CASE("PatternPlayer::setTempo changes the rate of subsequent advance() calls", "[PatternPlayer]") {
    sp404::PatternPlayer player;
    player.start(makeQuarterNotePattern(), 120.0, 48000.0); // 250 samples/tick

    std::vector<sp404::PatternTriggerEvent> events;
    // 24001, not 24000: an event exactly at the block's end tick is deferred to the *next* block
    // (see the "doesn't fire an event that lands exactly on the next block's start" test above),
    // so reaching event[1] (B, at tick 96 == sample 24000) within this call needs one extra
    // sample of headroom.
    player.advance(24001, events);
    REQUIRE(events.size() == 2); // A at tick 0, B at tick 96
    events.clear();

    player.setTempo(240.0); // double tempo -> half the samples/tick (125) from here on
    player.advance(125 * 96 + 1, events); // one more beat's worth of *new* samples/tick -> should reach event[2] (C)
    REQUIRE(events.size() == 1);
    CHECK(events[0].bank == 'C');
}

TEST_CASE("PatternPlayer::start falls back to the previous tempo for a non-positive bpm", "[PatternPlayer]") {
    sp404::PatternPlayer player;
    player.start(makeQuarterNotePattern(), 0.0, 48000.0); // invalid bpm -- must not divide by zero/hang
    REQUIRE(player.isPlaying());

    std::vector<sp404::PatternTriggerEvent> events;
    player.advance(384 * 250, events); // still fires using the default 120bpm (250 samples/tick)
    CHECK(events.size() == 4);
}
