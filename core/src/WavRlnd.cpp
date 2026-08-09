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

} // namespace sp404
