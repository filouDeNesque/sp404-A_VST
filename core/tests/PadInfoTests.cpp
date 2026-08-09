#include <algorithm>
#include <array>
#include <cstddef>
#include <stdexcept>

#include <catch2/catch_test_macros.hpp>

#include "sp404/PadInfo.h"

using sp404::PadInfo;

namespace {

// Hand-built fixture matching docs/sp404sx-format.md's byte table:
//   origSampleStart=0, origSampleEnd=4096, userSampleStart=16, userSampleEnd=4080,
//   volume=100, lofi=false, loop=true, gate=false, reverse=true,
//   format=Wave, channels=2, tempoMode=User, origTempo=1200 (120.0 BPM), userTempo=1400 (140.0 BPM)
constexpr std::array<uint8_t, PadInfo::encodedSize> kFixtureBytes{
    0x00, 0x00, 0x00, 0x00, // origSampleStart
    0x00, 0x00, 0x10, 0x00, // origSampleEnd
    0x00, 0x00, 0x00, 0x10, // userSampleStart
    0x00, 0x00, 0x0F, 0xF0, // userSampleEnd
    0x64,                   // volume
    0x00,                   // lofi
    0x01,                   // loop
    0x00,                   // gate
    0x01,                   // reverse
    0x01,                   // format (Wave)
    0x02,                   // channels
    0x02,                   // tempoMode (User)
    0x00, 0x00, 0x04, 0xB0, // origTempo
    0x00, 0x00, 0x05, 0x78, // userTempo
};

const std::byte* fixtureAsBytes() {
    return reinterpret_cast<const std::byte*>(kFixtureBytes.data());
}

} // namespace

TEST_CASE("PadInfo::decode reads fields at the documented offsets", "[PadInfo]") {
    const PadInfo info = PadInfo::decode(fixtureAsBytes(), PadInfo::encodedSize);

    CHECK(info.origSampleStart == 0);
    CHECK(info.origSampleEnd == 4096);
    CHECK(info.userSampleStart == 16);
    CHECK(info.userSampleEnd == 4080);
    CHECK(info.volume == 100);
    CHECK(info.lofi == false);
    CHECK(info.loop == true);
    CHECK(info.gate == false);
    CHECK(info.reverse == true);
    CHECK(info.format == PadInfo::Format::Wave);
    CHECK(info.channels == 2);
    CHECK(info.tempoMode == PadInfo::TempoMode::User);
    CHECK(info.origTempo == 1200);
    CHECK(info.userTempo == 1400);
}

TEST_CASE("PadInfo::encode round-trips through decode", "[PadInfo]") {
    const PadInfo original = PadInfo::decode(fixtureAsBytes(), PadInfo::encodedSize);

    std::array<std::byte, PadInfo::encodedSize> reencoded{};
    original.encode(reencoded.data(), reencoded.size());

    CHECK(std::equal(reencoded.begin(), reencoded.end(), fixtureAsBytes()));

    const PadInfo roundTripped = PadInfo::decode(reencoded.data(), reencoded.size());
    CHECK(roundTripped.origSampleStart == original.origSampleStart);
    CHECK(roundTripped.origTempo == original.origTempo);
    CHECK(roundTripped.userTempo == original.userTempo);
}

TEST_CASE("PadInfo::decode rejects the wrong buffer size", "[PadInfo]") {
    std::array<std::byte, 4> tooShort{};
    CHECK_THROWS_AS(PadInfo::decode(tooShort.data(), tooShort.size()), std::invalid_argument);
}
