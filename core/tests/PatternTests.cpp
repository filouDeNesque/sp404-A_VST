#include <algorithm>
#include <array>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "sp404/Pattern.h"
#include "sp404/SdCard.h"

namespace {

// Raw bytes of 3 real ROLAND/SP-404SX/PTN/PTNxxxxx.BIN files, dumped from a physical SP-404SX SD
// card -- this is what docs/sp404sx-format.md's PTN section was actually verified against (not
// just the community sources it cross-references). Only numeric pattern-event data (tick/note/
// velocity/length), no audio or user content, so embedding it here is safe.

// PTN00001.BIN, 232 bytes -- every real (non-placeholder) event resolves to bank C.
const std::vector<unsigned char> kPtnRealBankC = {
    0x0f, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x52, 0x52, 0x00, 0x00, 0x7f, 0x40, 0x00, 0x0f,
    0xff, 0x51, 0x40, 0x00, 0x7f, 0x40, 0x00, 0x0c, 0xff, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0xbd, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x04, 0x4c, 0x00, 0x00, 0x7f, 0x40, 0x00, 0x10,
    0xff, 0x51, 0x00, 0x00, 0x7f, 0x40, 0x00, 0x10, 0xff, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0xb7, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xff, 0x51, 0x00, 0x00, 0x7f, 0x40, 0x00, 0x12,
    0xff, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xfd, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x1c, 0x4e, 0x00, 0x00, 0x7f, 0x40, 0x00, 0x0e, 0xe3, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0xff,
    0xff, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xb2, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0xff, 0x4d, 0x40, 0x00, 0x7f, 0x40, 0x00, 0x0e, 0xff, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0xff, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xff, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0xff, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xff, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x1f, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xff, 0x4c, 0x40, 0x00, 0x7f, 0x40, 0x00, 0x12,
    0xff, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xff, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x6a, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x8c, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x0e, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};

// PTN00009.BIN, 344 bytes -- every real event resolves to bank D.
const std::vector<unsigned char> kPtnRealBankD = {
    0x10, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x44, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0xff,
    0x02, 0x5b, 0x00, 0x00, 0x7f, 0x40, 0x00, 0x0e, 0x30, 0x5d, 0x00, 0x00, 0x7f, 0x40, 0x00, 0x10,
    0x34, 0x5d, 0x00, 0x00, 0x7f, 0x40, 0x00, 0x15, 0x32, 0x5c, 0x00, 0x00, 0x7f, 0x40, 0x00, 0x0c,
    0x2d, 0x5d, 0x00, 0x00, 0x7f, 0x40, 0x00, 0x17, 0x05, 0x5b, 0x00, 0x00, 0x7f, 0x40, 0x00, 0x17,
    0x35, 0x5d, 0x00, 0x00, 0x7f, 0x40, 0x00, 0x12, 0x31, 0x5d, 0x00, 0x00, 0x7f, 0x40, 0x00, 0x15,
    0x2d, 0x5c, 0x00, 0x00, 0x7f, 0x40, 0x00, 0x10, 0x2e, 0x5d, 0x00, 0x00, 0x7f, 0x40, 0x00, 0x1b,
    0x05, 0x5b, 0x00, 0x00, 0x7f, 0x40, 0x00, 0x18, 0x2e, 0x5d, 0x00, 0x00, 0x7f, 0x40, 0x00, 0x15,
    0x33, 0x5d, 0x00, 0x00, 0x7f, 0x40, 0x00, 0x18, 0x2c, 0x5c, 0x00, 0x00, 0x7f, 0x40, 0x00, 0x10,
    0x31, 0x5d, 0x00, 0x00, 0x7f, 0x40, 0x00, 0x1a, 0x04, 0x5b, 0x00, 0x00, 0x7f, 0x40, 0x00, 0x14,
    0x30, 0x5d, 0x00, 0x00, 0x7f, 0x40, 0x00, 0x12, 0x31, 0x5d, 0x00, 0x00, 0x7f, 0x40, 0x00, 0x16,
    0x28, 0x5c, 0x00, 0x00, 0x7f, 0x40, 0x00, 0x0b, 0x2f, 0x5d, 0x00, 0x00, 0x7f, 0x40, 0x00, 0x16,
    0x01, 0x5b, 0x00, 0x00, 0x7f, 0x40, 0x00, 0x13, 0x2d, 0x5d, 0x00, 0x00, 0x7f, 0x40, 0x00, 0x13,
    0x31, 0x5d, 0x00, 0x00, 0x7f, 0x40, 0x00, 0x17, 0x30, 0x5c, 0x00, 0x00, 0x7f, 0x40, 0x00, 0x0b,
    0x2e, 0x5d, 0x00, 0x00, 0x7f, 0x40, 0x00, 0x14, 0x04, 0x5b, 0x00, 0x00, 0x7f, 0x40, 0x00, 0x11,
    0x2b, 0x5d, 0x00, 0x00, 0x7f, 0x40, 0x00, 0x0a, 0x33, 0x5d, 0x00, 0x00, 0x7f, 0x40, 0x00, 0x1a,
    0x2e, 0x5c, 0x00, 0x00, 0x7f, 0x40, 0x00, 0x0f, 0x2e, 0x5d, 0x00, 0x00, 0x7f, 0x40, 0x00, 0x18,
    0x05, 0x5b, 0x00, 0x00, 0x7f, 0x40, 0x00, 0x15, 0x2d, 0x5d, 0x00, 0x00, 0x7f, 0x40, 0x00, 0x11,
    0x30, 0x5d, 0x00, 0x00, 0x7f, 0x40, 0x00, 0x16, 0x2d, 0x5c, 0x00, 0x00, 0x7f, 0x40, 0x00, 0x10,
    0x2c, 0x5d, 0x00, 0x00, 0x7f, 0x40, 0x00, 0x17, 0x05, 0x5b, 0x00, 0x00, 0x7f, 0x40, 0x00, 0x0e,
    0x2c, 0x5d, 0x00, 0x00, 0x7f, 0x40, 0x00, 0x0c, 0x31, 0x5d, 0x00, 0x00, 0x7f, 0x40, 0x00, 0x15,
    0x0a, 0x5c, 0x00, 0x00, 0x7f, 0x40, 0x00, 0x09, 0x00, 0x8c, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};

// PTN00012.BIN, 296 bytes -- every real event resolves to bank I, all via bankSwitch == 65
// (the "second half", banks F-J) rather than the 0/64 seen in the other two fixtures above.
const std::vector<unsigned char> kPtnRealBankI = {
    0x5e, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x51, 0x58, 0x41, 0x00, 0x7f, 0x40, 0x00, 0x29,
    0x43, 0x58, 0x41, 0x00, 0x7f, 0x40, 0x00, 0x28, 0x5c, 0x58, 0x41, 0x00, 0x7f, 0x40, 0x00, 0x47,
    0x8c, 0x59, 0x41, 0x00, 0x7f, 0x40, 0x00, 0x66, 0x52, 0x5a, 0x41, 0x00, 0x7f, 0x40, 0x00, 0x32,
    0x4d, 0x5a, 0x41, 0x00, 0x7f, 0x40, 0x00, 0x2f, 0x62, 0x5a, 0x41, 0x00, 0x7f, 0x40, 0x00, 0x3e,
    0x3b, 0x59, 0x41, 0x00, 0x7f, 0x40, 0x00, 0x2c, 0x56, 0x59, 0x41, 0x00, 0x7f, 0x40, 0x00, 0x47,
    0x4b, 0x58, 0x41, 0x00, 0x7f, 0x40, 0x00, 0x34, 0x47, 0x58, 0x41, 0x00, 0x7f, 0x40, 0x00, 0x2d,
    0x60, 0x58, 0x41, 0x00, 0x7f, 0x40, 0x00, 0x45, 0x98, 0x5a, 0x41, 0x00, 0x7f, 0x40, 0x00, 0x6d,
    0x52, 0x59, 0x41, 0x00, 0x7f, 0x40, 0x00, 0x38, 0x48, 0x59, 0x41, 0x00, 0x7f, 0x40, 0x00, 0x37,
    0x53, 0x58, 0x41, 0x00, 0x7f, 0x40, 0x00, 0x32, 0x5a, 0x58, 0x41, 0x00, 0x7f, 0x40, 0x00, 0x44,
    0x36, 0x58, 0x41, 0x00, 0x7f, 0x40, 0x00, 0x23, 0xff, 0x58, 0x41, 0x00, 0x7f, 0x40, 0x00, 0x24,
    0xff, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xff, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0xff, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xff, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0xb2, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xd3, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0xff,
    0xff, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xff, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0xff, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xff, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0xff, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xff, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0xff, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xff, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x15, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x8c, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x0e, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};

std::filesystem::path writeFixture(const std::string& name, const std::vector<unsigned char>& bytes) {
    const auto path = std::filesystem::temp_directory_path() / name;
    std::ofstream out(path, std::ios::binary);
    out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    return path;
}

} // namespace

TEST_CASE("readPattern parses a real SP-404SX pattern (bank C, 14 bars)", "[Pattern]") {
    const auto path = writeFixture("sp404_core_test_ptn_bank_c.bin", kPtnRealBankC);
    const auto pattern = sp404::readPattern(path);
    REQUIRE(pattern.has_value());

    CHECK(pattern->events.size() == 27);
    CHECK(pattern->bars == 14);
    CHECK(pattern->timeSignature == 0);

    int realEvents = 0;
    for (const auto& event : pattern->events) {
        if (event.isPlaceholder()) {
            CHECK_FALSE(event.bank().has_value());
            CHECK_FALSE(event.padIndexInBank().has_value());
            continue;
        }
        ++realEvents;
        REQUIRE(event.bank().has_value());
        CHECK(*event.bank() == 'C');
        CHECK(event.velocity == 127);
        CHECK(event.unknown == 64);
        CHECK(event.pitchMode == 0);
    }
    CHECK(realEvents == 8);

    std::filesystem::remove(path);
}

TEST_CASE("totalTicks/absoluteEventTicks match real SP-404SX pattern data (bank C, 14 bars)",
          "[Pattern]") {
    const auto path = writeFixture("sp404_core_test_ptn_bank_c_ticks.bin", kPtnRealBankC);
    const auto pattern = sp404::readPattern(path);
    REQUIRE(pattern.has_value());

    CHECK(pattern->totalTicks() == 14 * sp404::kTicksPerBar);

    const auto ticks = sp404::absoluteEventTicks(*pattern);
    REQUIRE(ticks.size() == pattern->events.size());

    // Non-decreasing (every delta is a non-negative uint8_t).
    for (size_t i = 1; i < ticks.size(); ++i)
        CHECK(ticks[i] >= ticks[i - 1]);

    // The very first event (ticksSincePrevious = 0x0f = 15, see kPtnRealBankC) starts at tick 15,
    // not 0 -- there's a placeholder before the first real hit.
    CHECK(ticks[0] == 15);

    // Summing every event's delta (real + placeholder) lands exactly on the pattern's total
    // length -- this is the same arithmetic readPattern()'s bar-count derivation relies on (see
    // docs/sp404sx-format.md), re-checked here from the public API rather than the file's footer
    // byte directly.
    CHECK(ticks.back() == pattern->totalTicks());

    std::filesystem::remove(path);
}

TEST_CASE("readPattern parses a real SP-404SX pattern (bank D, 4 bars)", "[Pattern]") {
    const auto path = writeFixture("sp404_core_test_ptn_bank_d.bin", kPtnRealBankD);
    const auto pattern = sp404::readPattern(path);
    REQUIRE(pattern.has_value());

    CHECK(pattern->events.size() == 41);
    CHECK(pattern->bars == 4);

    int realEvents = 0;
    for (const auto& event : pattern->events) {
        if (event.isPlaceholder())
            continue;
        ++realEvents;
        REQUIRE(event.bank().has_value());
        CHECK(*event.bank() == 'D');
    }
    CHECK(realEvents == 39);

    std::filesystem::remove(path);
}

TEST_CASE("readPattern parses a real SP-404SX pattern using the F-J half (bank I, 14 bars)", "[Pattern]") {
    const auto path = writeFixture("sp404_core_test_ptn_bank_i.bin", kPtnRealBankI);
    const auto pattern = sp404::readPattern(path);
    REQUIRE(pattern.has_value());

    CHECK(pattern->events.size() == 35);
    CHECK(pattern->bars == 14);

    int realEvents = 0;
    for (const auto& event : pattern->events) {
        if (event.isPlaceholder())
            continue;
        ++realEvents;
        CHECK(event.bankSwitch == 65); // this fixture only ever uses the 65 spelling, not 1
        REQUIRE(event.bank().has_value());
        CHECK(*event.bank() == 'I');
    }
    CHECK(realEvents == 19);

    std::filesystem::remove(path);
}

TEST_CASE("readPattern returns nullopt for a missing file", "[Pattern]") {
    CHECK_FALSE(sp404::readPattern("/nonexistent/path/PTN99999.BIN").has_value());
}

TEST_CASE("readPattern returns nullopt for a file too short to hold the footer", "[Pattern]") {
    const std::vector<unsigned char> tooShort(10, 0);
    const auto path = writeFixture("sp404_core_test_ptn_too_short.bin", tooShort);
    CHECK_FALSE(sp404::readPattern(path).has_value());
    std::filesystem::remove(path);
}

TEST_CASE("readPattern returns nullopt for a size that isn't footer + whole events", "[Pattern]") {
    const std::vector<unsigned char> misaligned(16 + 8 + 3, 0); // footer + 1 event + 3 stray bytes
    const auto path = writeFixture("sp404_core_test_ptn_misaligned.bin", misaligned);
    CHECK_FALSE(sp404::readPattern(path).has_value());
    std::filesystem::remove(path);
}

TEST_CASE("PatternEvent::bank/padIndexInBank cover both bankSwitch spellings and edge cases", "[Pattern]") {
    using sp404::PatternEvent;

    // A1: lowest real midiNote (47), bankSwitch 0.
    PatternEvent a1;
    a1.midiNote = 47;
    a1.bankSwitch = 0;
    REQUIRE(a1.bank().has_value());
    CHECK(*a1.bank() == 'A');
    REQUIRE(a1.padIndexInBank().has_value());
    CHECK(*a1.padIndexInBank() == 1);
    CHECK(a1.sampleIndex0to119() == 0);

    // E12: highest midiNote (106) in the first half, bankSwitch 64 spelling.
    PatternEvent e12;
    e12.midiNote = 106;
    e12.bankSwitch = 64;
    REQUIRE(e12.bank().has_value());
    CHECK(*e12.bank() == 'E');
    CHECK(*e12.padIndexInBank() == 12);
    CHECK(e12.sampleIndex0to119() == 59);

    // F1: lowest midiNote (47) in the second half via the "1" spelling (not yet seen on a real
    // pattern in this session, but documented -- see docs/sp404sx-format.md).
    PatternEvent f1;
    f1.midiNote = 47;
    f1.bankSwitch = 1;
    REQUIRE(f1.bank().has_value());
    CHECK(*f1.bank() == 'F');
    CHECK(*f1.padIndexInBank() == 1);
    CHECK(f1.sampleIndex0to119() == 60);

    // J12: highest midiNote (106) in the second half via the "65" spelling.
    PatternEvent j12;
    j12.midiNote = 106;
    j12.bankSwitch = 65;
    REQUIRE(j12.bank().has_value());
    CHECK(*j12.bank() == 'J');
    CHECK(*j12.padIndexInBank() == 12);
    CHECK(j12.sampleIndex0to119() == 119);

    // Placeholder/rest event: midiNote 128 always means "nothing here", regardless of the other
    // fields.
    PatternEvent placeholder;
    placeholder.midiNote = 128;
    placeholder.bankSwitch = 0;
    CHECK(placeholder.isPlaceholder());
    CHECK_FALSE(placeholder.bank().has_value());
    CHECK_FALSE(placeholder.padIndexInBank().has_value());

    // Unrecognized bankSwitch value: fail closed (nullopt) rather than guess.
    PatternEvent unknownSwitch;
    unknownSwitch.midiNote = 50;
    unknownSwitch.bankSwitch = 12;
    CHECK_FALSE(unknownSwitch.bank().has_value());
    CHECK_FALSE(unknownSwitch.padIndexInBank().has_value());

    // midiNote out of the 47-106 range for its half: fail closed too.
    PatternEvent outOfRange;
    outOfRange.midiNote = 200;
    outOfRange.bankSwitch = 0;
    CHECK_FALSE(outOfRange.bank().has_value());
}

TEST_CASE("PatternEvent::encode round-trips through decode", "[Pattern]") {
    sp404::PatternEvent event;
    event.ticksSincePrevious = 200;
    event.midiNote = 88;
    event.bankSwitch = 65;
    event.pitchMode = 0;
    event.velocity = 100;
    event.unknown = 64;
    event.lengthTicks = 12345;

    std::array<std::byte, sp404::PatternEvent::encodedSize> bytes{};
    event.encode(bytes.data(), bytes.size());
    const auto decoded = sp404::PatternEvent::decode(bytes.data(), bytes.size());

    CHECK(decoded.ticksSincePrevious == event.ticksSincePrevious);
    CHECK(decoded.midiNote == event.midiNote);
    CHECK(decoded.bankSwitch == event.bankSwitch);
    CHECK(decoded.pitchMode == event.pitchMode);
    CHECK(decoded.velocity == event.velocity);
    CHECK(decoded.unknown == event.unknown);
    CHECK(decoded.lengthTicks == event.lengthTicks);
}

TEST_CASE("encode(Pattern) reproduces the real bank-C fixture byte-for-byte", "[Pattern]") {
    const auto path = writeFixture("sp404_core_test_ptn_bank_c_encode.bin", kPtnRealBankC);
    const auto pattern = sp404::readPattern(path);
    REQUIRE(pattern.has_value());

    const auto reencoded = sp404::encode(*pattern);
    REQUIRE(reencoded.size() == kPtnRealBankC.size());

    std::vector<unsigned char> reencodedUnsigned(reencoded.size());
    std::transform(reencoded.begin(), reencoded.end(), reencodedUnsigned.begin(),
                    [](std::byte b) { return static_cast<unsigned char>(b); });
    CHECK(reencodedUnsigned == kPtnRealBankC);

    std::filesystem::remove(path);
}

TEST_CASE("makeNoteEvent matches real hardware's encoding at both ends of the pad range", "[Pattern]") {
    // A1 (see docs/sp404sx-format.md): MidiNote=47, BankSwitch=0.
    const auto a1 = sp404::makeNoteEvent('A', 1, 10, 127, 500);
    CHECK(a1.midiNote == 47);
    CHECK(a1.bankSwitch == 0);
    CHECK(a1.unknown == 64);
    CHECK(a1.pitchMode == 0);
    CHECK(a1.ticksSincePrevious == 10);
    CHECK(a1.velocity == 127);
    CHECK(a1.lengthTicks == 500);
    REQUIRE(a1.bank().has_value());
    CHECK(*a1.bank() == 'A');
    CHECK(*a1.padIndexInBank() == 1);

    // J12: MidiNote=106, BankSwitch=1 (the "1" spelling -- see makeNoteEvent's doc comment).
    const auto j12 = sp404::makeNoteEvent('J', 12, 0, 1, 0);
    CHECK(j12.midiNote == 106);
    CHECK(j12.bankSwitch == 1);
    REQUIRE(j12.bank().has_value());
    CHECK(*j12.bank() == 'J');
    CHECK(*j12.padIndexInBank() == 12);

    // Round-trip every real pad through makeNoteEvent -> bank()/padIndexInBank().
    for (char bank = 'A'; bank <= 'J'; ++bank) {
        for (int pad = 1; pad <= 12; ++pad) {
            const auto event = sp404::makeNoteEvent(bank, pad, 5, 100, 50);
            REQUIRE(event.bank().has_value());
            REQUIRE(event.padIndexInBank().has_value());
            CHECK(*event.bank() == bank);
            CHECK(*event.padIndexInBank() == pad);
        }
    }

    CHECK_THROWS_AS(sp404::makeNoteEvent('Z', 1, 0, 0, 0), std::invalid_argument);
    CHECK_THROWS_AS(sp404::makeNoteEvent('A', 13, 0, 0, 0), std::invalid_argument);
}

TEST_CASE("makePlaceholderEvent matches the real placeholder shape", "[Pattern]") {
    const auto placeholder = sp404::makePlaceholderEvent(200);
    CHECK(placeholder.isPlaceholder());
    CHECK(placeholder.ticksSincePrevious == 200);
    CHECK(placeholder.midiNote == 128);
    CHECK(placeholder.bankSwitch == 0);
    CHECK(placeholder.pitchMode == 0);
    CHECK(placeholder.velocity == 0);
    CHECK(placeholder.unknown == 0);
    CHECK(placeholder.lengthTicks == 0);
}

TEST_CASE("writePattern writes bytes readPattern can read back", "[Pattern]") {
    const auto root = std::filesystem::temp_directory_path() / "sp404_core_test_write_pattern";
    std::filesystem::remove_all(root);

    sp404::Pattern pattern;
    pattern.events.push_back(sp404::makeNoteEvent('B', 3, 0, 110, 200));
    pattern.events.push_back(sp404::makeNoteEvent('B', 5, 96, 90, 150));
    pattern.events.push_back(sp404::makePlaceholderEvent(238));
    pattern.bars = 1;
    pattern.timeSignature = 0;

    sp404::writePattern(root, 'A', 4, pattern);

    const auto readBack = sp404::readPattern(sp404::patternSlotPath(root, 'A', 4));
    REQUIRE(readBack.has_value());
    CHECK(readBack->bars == 1);
    CHECK(readBack->events.size() == 3);
    REQUIRE(readBack->events[0].bank().has_value());
    CHECK(*readBack->events[0].bank() == 'B');
    CHECK(*readBack->events[0].padIndexInBank() == 3);
    REQUIRE(readBack->events[1].bank().has_value());
    CHECK(*readBack->events[1].padIndexInBank() == 5);
    CHECK(readBack->events[2].isPlaceholder());

    CHECK_THROWS_AS(sp404::writePattern(root, 'Z', 1, pattern), std::invalid_argument);

    std::filesystem::remove_all(root);
}
