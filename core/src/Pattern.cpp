#include "sp404/Pattern.h"

#include <fstream>
#include <iterator>
#include <stdexcept>

#include "sp404/SdCard.h"

namespace sp404 {

namespace {

uint16_t readU16BE(const std::byte* p) {
    return static_cast<uint16_t>((static_cast<uint16_t>(p[0]) << 8) | static_cast<uint16_t>(p[1]));
}

void writeU16BE(std::byte* p, uint16_t value) {
    p[0] = static_cast<std::byte>((value >> 8) & 0xFF);
    p[1] = static_cast<std::byte>(value & 0xFF);
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

std::vector<int> absoluteEventTicks(const Pattern& pattern) {
    std::vector<int> ticks;
    ticks.reserve(pattern.events.size());
    int running = 0;
    for (const auto& event : pattern.events) {
        running += event.ticksSincePrevious;
        ticks.push_back(running);
    }
    return ticks;
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

void PatternEvent::encode(std::byte* out, size_t size) const {
    if (out == nullptr || size != encodedSize)
        throw std::invalid_argument("PatternEvent::encode requires exactly encodedSize bytes");

    out[0] = static_cast<std::byte>(ticksSincePrevious);
    out[1] = static_cast<std::byte>(midiNote);
    out[2] = static_cast<std::byte>(bankSwitch);
    out[3] = static_cast<std::byte>(pitchMode);
    out[4] = static_cast<std::byte>(velocity);
    out[5] = static_cast<std::byte>(unknown);
    writeU16BE(out + 6, lengthTicks);
}

PatternEvent makeNoteEvent(char bank, int padIndexInBank, uint8_t ticksSincePrevious, uint8_t velocity,
                            uint16_t lengthTicks) {
    if (bank < 'A' || bank > 'J' || padIndexInBank < 1 || padIndexInBank > kPadsPerBank)
        throw std::invalid_argument("makeNoteEvent: bank/padIndexInBank out of range");

    const int sampleIndex = (bank - 'A') * kPadsPerBank + (padIndexInBank - 1);
    const int half = sampleIndex >= kPadsPerHalf ? 1 : 0;
    const int sampleNumberInHalf = (sampleIndex % kPadsPerHalf) + 1; // 1-based

    PatternEvent event;
    event.ticksSincePrevious = ticksSincePrevious;
    event.midiNote = static_cast<uint8_t>(46 + sampleNumberInHalf);
    event.bankSwitch = static_cast<uint8_t>(half);
    event.pitchMode = 0;
    event.velocity = velocity;
    event.unknown = 64;
    event.lengthTicks = lengthTicks;
    return event;
}

PatternEvent makePlaceholderEvent(uint8_t ticksSincePrevious) {
    PatternEvent event;
    event.ticksSincePrevious = ticksSincePrevious;
    event.midiNote = 128;
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
    // 3 real patterns: sum(every event's ticksSincePrevious) / kTicksPerBar lands exactly on this
    // byte's value every time. This differs from the MKii/OG layouts
    // documented elsewhere (which use footer bytes 8 and 14 for the bar count) -- see
    // docs/sp404sx-format.md.
    const auto* footer = reinterpret_cast<const std::byte*>(bytes.data() + numEvents * PatternEvent::encodedSize);
    pattern.bars = static_cast<int>(footer[9]);
    pattern.timeSignature = static_cast<int>(footer[12]);

    return pattern;
}

std::vector<std::byte> encode(const Pattern& pattern) {
    std::vector<std::byte> bytes(pattern.events.size() * PatternEvent::encodedSize + kFooterSize);

    for (size_t i = 0; i < pattern.events.size(); ++i)
        pattern.events[i].encode(bytes.data() + i * PatternEvent::encodedSize, PatternEvent::encodedSize);

    // See docs/sp404sx-format.md's footer table: byte 1 = 140 on every real pattern checked so
    // far, byte 9 = bar count (SP-404SX-specific -- byte 8 is unused, unlike the MKii/OG layouts
    // that store it there), byte 12 = time signature. Every other byte is 0.
    std::byte* footer = bytes.data() + pattern.events.size() * PatternEvent::encodedSize;
    footer[1] = std::byte{140};
    footer[9] = static_cast<std::byte>(pattern.bars);
    footer[12] = static_cast<std::byte>(pattern.timeSignature);

    return bytes;
}

void writePattern(const std::filesystem::path& sdRoot, char bankName, int indexInBank, const Pattern& pattern) {
    const auto path = patternSlotPath(sdRoot, bankName, indexInBank); // throws if out of range
    std::filesystem::create_directories(path.parent_path());

    const auto bytes = encode(pattern);
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out)
        throw std::runtime_error("Cannot open " + path.string() + " for writing");
    out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (!out)
        throw std::runtime_error("Failed writing " + path.string());
}

} // namespace sp404
