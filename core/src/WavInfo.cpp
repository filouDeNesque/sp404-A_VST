#include "sp404/WavInfo.h"

#include <array>
#include <cstring>
#include <fstream>

namespace sp404 {

namespace {

uint32_t readU32LE(const unsigned char* p) {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

uint16_t readU16LE(const unsigned char* p) {
    return static_cast<uint16_t>(static_cast<uint16_t>(p[0]) | (static_cast<uint16_t>(p[1]) << 8));
}

} // namespace

double WavInfo::durationSeconds() const {
    const double bytesPerFrame = channels * (bitsPerSample / 8.0);
    if (sampleRate == 0 || bytesPerFrame <= 0.0)
        return 0.0;
    return (dataSize / bytesPerFrame) / sampleRate;
}

std::optional<WavInfo> readWavInfo(const std::filesystem::path& wavPath) {
    std::ifstream file(wavPath, std::ios::binary);
    if (!file)
        return std::nullopt;

    std::array<unsigned char, 12> riffHeader{};
    file.read(reinterpret_cast<char*>(riffHeader.data()), static_cast<std::streamsize>(riffHeader.size()));
    if (!file || std::memcmp(riffHeader.data(), "RIFF", 4) != 0 ||
        std::memcmp(riffHeader.data() + 8, "WAVE", 4) != 0)
        return std::nullopt;

    WavInfo info;
    bool haveFmt = false;
    bool haveData = false;

    std::array<unsigned char, 8> chunkHeader{};
    while (file.read(reinterpret_cast<char*>(chunkHeader.data()), static_cast<std::streamsize>(chunkHeader.size()))) {
        const uint32_t chunkSize = readU32LE(chunkHeader.data() + 4);
        const auto seekAmount = static_cast<std::streamoff>(chunkSize) + static_cast<std::streamoff>(chunkSize % 2);

        if (std::memcmp(chunkHeader.data(), "fmt ", 4) == 0 && chunkSize >= 16) {
            std::array<unsigned char, 16> fmt{};
            file.read(reinterpret_cast<char*>(fmt.data()), static_cast<std::streamsize>(fmt.size()));
            if (!file)
                break;
            info.channels = readU16LE(fmt.data() + 2);
            info.sampleRate = readU32LE(fmt.data() + 4);
            info.bitsPerSample = readU16LE(fmt.data() + 14);
            haveFmt = true;

            file.seekg(seekAmount - 16, std::ios::cur);
        } else if (std::memcmp(chunkHeader.data(), "data", 4) == 0) {
            info.dataSize = chunkSize;
            info.dataOffset = static_cast<uint32_t>(file.tellg());
            haveData = true;
            file.seekg(seekAmount, std::ios::cur);
        } else {
            file.seekg(seekAmount, std::ios::cur);
        }

        if (haveFmt && haveData)
            break;
    }

    if (!haveFmt || !haveData)
        return std::nullopt;

    return info;
}

} // namespace sp404
