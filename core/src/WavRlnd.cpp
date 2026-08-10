#include "sp404/WavRlnd.h"

#include <stdexcept>

namespace sp404 {

RlndChunk RlndChunk::decode(const std::byte* data, size_t size) {
    if (data == nullptr || size != encodedSize)
        throw std::invalid_argument("RlndChunk::decode requires exactly encodedSize bytes");

    RlndChunk chunk;
    for (size_t i = 0; i < deviceSize; ++i)
        chunk.device[i] = static_cast<char>(data[i]);
    chunk.unknown1 = static_cast<uint8_t>(data[deviceSize + 0]);
    chunk.unknown2 = static_cast<uint8_t>(data[deviceSize + 1]);
    chunk.unknown3 = static_cast<uint8_t>(data[deviceSize + 2]);
    chunk.unknown4 = static_cast<uint8_t>(data[deviceSize + 3]);
    chunk.sampleIndex = static_cast<uint8_t>(data[deviceSize + 4]);
    return chunk;
}

void RlndChunk::encode(std::byte* out, size_t size) const {
    if (out == nullptr || size != encodedSize)
        throw std::invalid_argument("RlndChunk::encode requires exactly encodedSize bytes");

    for (size_t i = 0; i < deviceSize; ++i)
        out[i] = static_cast<std::byte>(device[i]);
    out[deviceSize + 0] = static_cast<std::byte>(unknown1);
    out[deviceSize + 1] = static_cast<std::byte>(unknown2);
    out[deviceSize + 2] = static_cast<std::byte>(unknown3);
    out[deviceSize + 3] = static_cast<std::byte>(unknown4);
    out[deviceSize + 4] = static_cast<std::byte>(sampleIndex);
}

namespace {

void appendAscii4(std::vector<std::byte>& out, const char (&fourCC)[5]) {
    for (int i = 0; i < 4; ++i)
        out.push_back(static_cast<std::byte>(fourCC[i]));
}

void appendU32LE(std::vector<std::byte>& out, uint32_t value) {
    for (int i = 0; i < 4; ++i)
        out.push_back(static_cast<std::byte>((value >> (8 * i)) & 0xFF));
}

void appendU16LE(std::vector<std::byte>& out, uint16_t value) {
    for (int i = 0; i < 2; ++i)
        out.push_back(static_cast<std::byte>((value >> (8 * i)) & 0xFF));
}

} // namespace

std::vector<std::byte> encodeWavWithRlndChunk(std::span<const std::byte> pcmData, int numChannels,
                                               uint32_t sampleRate, const RlndChunk& rlnd) {
    if (numChannels <= 0 || sampleRate == 0)
        return {};

    constexpr uint16_t kBitsPerSample = 16;
    constexpr uint32_t kDataChunkOffset = 512; // verified against a real card, see docs/sp404sx-format.md

    const auto blockAlign = static_cast<uint16_t>(numChannels * (kBitsPerSample / 8));
    const uint32_t byteRate = sampleRate * blockAlign;

    std::vector<std::byte> out;
    out.reserve(static_cast<size_t>(kDataChunkOffset) + pcmData.size() + 1);

    appendAscii4(out, "RIFF");
    appendU32LE(out, 0); // patched below once the final size is known
    appendAscii4(out, "WAVE");

    appendAscii4(out, "fmt ");
    appendU32LE(out, 18); // 16 standard PCM fields + 2-byte zero cbSize extension, matches real card
    appendU16LE(out, 1);  // audioFormat = PCM
    appendU16LE(out, static_cast<uint16_t>(numChannels));
    appendU32LE(out, sampleRate);
    appendU32LE(out, byteRate);
    appendU16LE(out, blockAlign);
    appendU16LE(out, kBitsPerSample);
    appendU16LE(out, 0); // cbSize

    // out.size() here is always 12 (RIFF header) + 26 (fmt chunk, header+data) = 38, regardless
    // of numChannels/sampleRate -- so rlndChunkSize is always 512 - 38 - 8 (RLND header) - 8
    // (data header) = 458 for this exact layout, computed rather than hardcoded to keep that
    // derivation visible in code.
    const size_t sizeBeforeRlnd = out.size();
    appendAscii4(out, "RLND");
    const size_t rlndChunkSize = static_cast<size_t>(kDataChunkOffset) - sizeBeforeRlnd - 8 - 8;
    appendU32LE(out, static_cast<uint32_t>(rlndChunkSize));

    std::array<std::byte, RlndChunk::encodedSize> rlndPayload{};
    rlnd.encode(rlndPayload.data(), rlndPayload.size());
    out.insert(out.end(), rlndPayload.begin(), rlndPayload.end());
    out.resize(out.size() + (rlndChunkSize - rlndPayload.size()), std::byte{0});

    appendAscii4(out, "data");
    appendU32LE(out, static_cast<uint32_t>(pcmData.size()));
    // out.size() == kDataChunkOffset exactly here -- the whole point of the padding above.
    out.insert(out.end(), pcmData.begin(), pcmData.end());
    if (pcmData.size() % 2 != 0)
        out.push_back(std::byte{0}); // RIFF chunks are word-aligned

    const auto riffSize = static_cast<uint32_t>(out.size() - 8);
    out[4] = static_cast<std::byte>(riffSize & 0xFF);
    out[5] = static_cast<std::byte>((riffSize >> 8) & 0xFF);
    out[6] = static_cast<std::byte>((riffSize >> 16) & 0xFF);
    out[7] = static_cast<std::byte>((riffSize >> 24) & 0xFF);

    return out;
}

} // namespace sp404
