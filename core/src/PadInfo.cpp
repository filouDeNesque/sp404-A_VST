#include "sp404/PadInfo.h"

#include <stdexcept>

namespace sp404 {

namespace {

uint32_t readU32BE(const std::byte* p) {
    return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) |
           (static_cast<uint32_t>(p[2]) << 8) | static_cast<uint32_t>(p[3]);
}

void writeU32BE(std::byte* p, uint32_t value) {
    p[0] = static_cast<std::byte>((value >> 24) & 0xFF);
    p[1] = static_cast<std::byte>((value >> 16) & 0xFF);
    p[2] = static_cast<std::byte>((value >> 8) & 0xFF);
    p[3] = static_cast<std::byte>(value & 0xFF);
}

} // namespace

PadInfo PadInfo::decode(const std::byte* data, size_t size) {
    if (data == nullptr || size != encodedSize)
        throw std::invalid_argument("PadInfo::decode requires exactly encodedSize bytes");

    PadInfo info;
    info.origSampleStart = readU32BE(data + 0);
    info.origSampleEnd = readU32BE(data + 4);
    info.userSampleStart = readU32BE(data + 8);
    info.userSampleEnd = readU32BE(data + 12);
    info.volume = static_cast<uint8_t>(data[16]);
    info.lofi = static_cast<uint8_t>(data[17]) != 0;
    info.loop = static_cast<uint8_t>(data[18]) != 0;
    info.gate = static_cast<uint8_t>(data[19]) != 0;
    info.reverse = static_cast<uint8_t>(data[20]) != 0;
    info.format = static_cast<Format>(data[21]);
    info.channels = static_cast<uint8_t>(data[22]);
    info.tempoMode = static_cast<TempoMode>(data[23]);
    info.origTempo = readU32BE(data + 24);
    info.userTempo = readU32BE(data + 28);
    return info;
}

void PadInfo::encode(std::byte* out, size_t size) const {
    if (out == nullptr || size != encodedSize)
        throw std::invalid_argument("PadInfo::encode requires exactly encodedSize bytes");

    writeU32BE(out + 0, origSampleStart);
    writeU32BE(out + 4, origSampleEnd);
    writeU32BE(out + 8, userSampleStart);
    writeU32BE(out + 12, userSampleEnd);
    out[16] = static_cast<std::byte>(volume);
    out[17] = static_cast<std::byte>(lofi ? 1 : 0);
    out[18] = static_cast<std::byte>(loop ? 1 : 0);
    out[19] = static_cast<std::byte>(gate ? 1 : 0);
    out[20] = static_cast<std::byte>(reverse ? 1 : 0);
    out[21] = static_cast<std::byte>(format);
    out[22] = static_cast<std::byte>(channels);
    out[23] = static_cast<std::byte>(tempoMode);
    writeU32BE(out + 24, origTempo);
    writeU32BE(out + 28, userTempo);
}

} // namespace sp404
