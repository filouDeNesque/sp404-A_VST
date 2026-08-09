#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <vector>

namespace sp404 {

// 96 PPQN * 4 beats/bar in 4/4 -- see docs/sp404sx-format.md and readPattern()'s bar-count
// derivation, which this same value was reverse-engineered from.
inline constexpr int kTicksPerBar = 384;

// One event decoded from a Roland SP-404SX ROLAND/SP-404SX/PTN/PTNxxxxx.BIN pattern file. 8
// bytes on disk; unlike the rest of this codebase's Roland-format parsing (PadInfo, RlndChunk,
// which are big-endian 32-bit fields), the only multi-byte field here is `lengthTicks`, and it's
// big-endian too -- verified directly against a real card, see docs/sp404sx-format.md.
//
// VERIFIED against 3 real pattern files read off a physical SP-404SX SD card (not just the
// community docs this was cross-checked with) -- see docs/sp404sx-format.md for exactly what was
// confirmed vs still assumed. In particular: pad addressing (bankSwitch/midiNote -> bank+pad) and
// the tick/bar arithmetic are confirmed; `pitchMode` (Step Sequencer pitch shift, only nonzero in
// modes this parser hasn't seen real data for) and `unknown` (constant 64 on every real event
// seen) are carried through unparsed.
struct PatternEvent {
    static constexpr size_t encodedSize = 8;

    uint8_t ticksSincePrevious = 0; // delay since the previous event, in pattern ticks (96 PPQN, i.e. 384 ticks/bar in 4/4)
    uint8_t midiNote = 0;           // 47-106 for a real note, or 128 for a placeholder/rest (see isPlaceholder)
    uint8_t bankSwitch = 0;         // 0 or 64 = banks A-E; 1 or 65 = banks F-J (both spellings seen on real patterns)
    uint8_t pitchMode = 0;          // Step Sequencer pitch shift; 0 outside that mode (the only value seen so far)
    uint8_t velocity = 0;           // 0-127
    uint8_t unknown = 0;            // observed constant 64 (0x40) on every real, non-placeholder event so far
    uint16_t lengthTicks = 0;       // how long the pad is held, in ticks

    bool isPlaceholder() const { return midiNote == 128; }

    // 0-based combined bank+pad index (0 == A1, +12 per bank, up to 119 == J12) -- same
    // convention as RlndChunk::sampleIndex. std::nullopt for a placeholder event or a bankSwitch
    // value other than the four listed above.
    std::optional<int> sampleIndex0to119() const;
    // 'A'-'J', or std::nullopt (see sampleIndex0to119).
    std::optional<char> bank() const;
    // 1-based pad index within its bank (1-12), or std::nullopt (see sampleIndex0to119).
    std::optional<int> padIndexInBank() const;

    // data must point at exactly encodedSize bytes.
    static PatternEvent decode(const std::byte* data, size_t size);
};

struct Pattern {
    std::vector<PatternEvent> events;
    int bars = 0;          // whole bars in the pattern -- see docs/sp404sx-format.md
    int timeSignature = 0; // 0=4/4, 1=3/4, 2=2/4, 3=1/4, 4=5/4, 5=6/4, 7=7/4 (raw stored byte)

    // Total pattern length in ticks (bars * kTicksPerBar) -- the denominator for positioning
    // events on a timeline (see absoluteEventTicks below).
    int totalTicks() const { return bars * kTicksPerBar; }
};

// Absolute tick position of each event in pattern.events, same order and count -- computed as the
// cumulative sum of each event's ticksSincePrevious, since the raw format only encodes the delay
// since the previous event, not an absolute position. Useful for rendering a timeline preview.
// The last entry (which may be a placeholder) always equals pattern.totalTicks() on every real
// pattern verified so far (see docs/sp404sx-format.md).
std::vector<int> absoluteEventTicks(const Pattern& pattern);

// Returns std::nullopt if patternPath isn't readable, is shorter than the 16-byte footer, or its
// size isn't footer + a whole number of PatternEvent::encodedSize-byte records.
std::optional<Pattern> readPattern(const std::filesystem::path& patternPath);

} // namespace sp404
