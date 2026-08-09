#include <array>
#include <cstddef>
#include <stdexcept>
#include <string>

#include <catch2/catch_test_macros.hpp>

#include "sp404/WavRlnd.h"

using sp404::RlndChunk;

TEST_CASE("RlndChunk::encode then decode round-trips device and sampleIndex", "[WavRlnd]") {
    RlndChunk chunk;
    chunk.sampleIndex = 25; // bank C ('A' + 2 banks * 12), pad 2 -> index 25

    std::array<std::byte, RlndChunk::encodedSize> buffer{};
    chunk.encode(buffer.data(), buffer.size());

    const RlndChunk decoded = RlndChunk::decode(buffer.data(), buffer.size());

    CHECK(decoded.device == chunk.device);
    CHECK(decoded.sampleIndex == 25);
    CHECK(decoded.unknown1 == chunk.unknown1);
    CHECK(decoded.unknown2 == chunk.unknown2);
    CHECK(decoded.unknown3 == chunk.unknown3);
    CHECK(decoded.unknown4 == chunk.unknown4);
}

TEST_CASE("RlndChunk default device identifies the SP-404SX", "[WavRlnd]") {
    const RlndChunk chunk;
    const std::string device(chunk.device.begin(), chunk.device.end());
    CHECK(device == "roifspsx");
}

TEST_CASE("RlndChunk::decode rejects the wrong buffer size", "[WavRlnd]") {
    std::array<std::byte, 4> tooShort{};
    CHECK_THROWS_AS(RlndChunk::decode(tooShort.data(), tooShort.size()), std::invalid_argument);
}
