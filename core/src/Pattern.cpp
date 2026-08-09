#include "sp404/Pattern.h"

#include <fstream>
#include <iterator>
#include <stdexcept>

namespace sp404 {

namespace {

uint16_t readU16BE(const std::byte* p) {
    return static_cast<uint16_t>((static_cast<uint16_t>(p[0]) << 8) | static_cast<uint16_t>(p[1]));
}

constexpr size_t kFooterSize = 16;
constexpr int kPadsPerBank = 12; // SP-404SX; the MKii's PTN format uses 16 and isn't handled here
constexpr int kPadsPerHalf = kPadsPerBank * 5; // banks A-E or F-J

} // namespace

std::optional<int> PatternEvent::sampleIndex0to119() const {
    if (isPlaceholder())
        return std::nullopt;

    int half;
    if (bankSwitch == 0 || bankSwitch == 64)
        half = 0;
    else if (bankSwitch == 1 || bankSwitch == 65)
        half = 1;
    else
        return std::nullopt;

    const int sampleNumber = static_cast<int>(midiNote) - 46; // 1-based within its half
    if (sampleNumber < 1 || sampleNumber > kPadsPerHalf)
        return std::nullopt;

    return (sampleNumber - 1) + half * kPadsPerHalf;
}

std::optional<char> PatternEvent::bank() const {
    const auto index = sampleIndex0to119();
    if (!index)
        return std::nullopt;
    return static_cast<char>('A' + (*index / kPadsPerBank));
}

std::optional<int> PatternEvent::padIndexInBank() const {
    const auto index = sampleIndex0to119();
    if (!index)
        return std::nullopt;
    return (*index % kPadsPerBank) + 1;
}

PatternEvent PatternEvent::decode(const std::byte* data, size_t size) {
    if (data == nullptr || size != encodedSize)
        throw std::invalid_argument("PatternEvent::decode requires exactly encodedSize bytes");

    PatternEvent event;
    event.ticksSincePrevious = static_cast<uint8_t>(data[0]);
    event.midiNote = static_cast<uint8_t>(data[1]);
    event.bankSwitch = static_cast<uint8_t>(data[2]);
    event.pitchMode = static_cast<uint8_t>(data[3]);
    event.velocity = static_cast<uint8_t>(data[4]);
    event.unknown = static_cast<uint8_t>(data[5]);
    event.lengthTicks = readU16BE(data + 6);
    return event;
}

std::optional<Pattern> readPattern(const std::filesystem::path& patternPath) {
    std::ifstream file(patternPath, std::ios::binary);
    if (!file)
        return std::nullopt;

    const std::vector<unsigned char> bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    if (bytes.size() < kFooterSize || (bytes.size() - kFooterSize) % PatternEvent::encodedSize != 0)
        return std::nullopt;

    const size_t numEvents = (bytes.size() - kFooterSize) / PatternEvent::encodedSize;

    Pattern pattern;
    pattern.events.reserve(numEvents);
    for (size_t i = 0; i < numEvents; ++i) {
        const auto* eventBytes = reinterpret_cast<const std::byte*>(bytes.data() + i * PatternEvent::encodedSize);
        pattern.events.push_back(PatternEvent::decode(eventBytes, PatternEvent::encodedSize));
    }

    // Byte 9 of the 16-byte footer holds the whole bar count on the SP-404SX -- verified against
    // 3 real patterns: sum(every event's ticksSincePrevious) / 384 (= 96 PPQN * 4 beats/bar in
    // 4/4) lands exactly on this byte's value every time. This differs from the MKii/OG layouts
    // documented elsewhere (which use footer bytes 8 and 14 for the bar count) -- see
    // docs/sp404sx-format.md.
    const auto* footer = reinterpret_cast<const std::byte*>(bytes.data() + numEvents * PatternEvent::encodedSize);
    pattern.bars = static_cast<int>(footer[9]);
    pattern.timeSignature = static_cast<int>(footer[12]);

    return pattern;
}

} // namespace sp404
