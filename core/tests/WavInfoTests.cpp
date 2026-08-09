#include <cstdint>
#include <filesystem>
#include <fstream>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "sp404/WavInfo.h"

namespace {

void writeU32LE(std::ofstream& out, uint32_t v) {
    const unsigned char b[4]{static_cast<unsigned char>(v & 0xFF), static_cast<unsigned char>((v >> 8) & 0xFF),
                              static_cast<unsigned char>((v >> 16) & 0xFF), static_cast<unsigned char>((v >> 24) & 0xFF)};
    out.write(reinterpret_cast<const char*>(b), 4);
}

void writeU16LE(std::ofstream& out, uint16_t v) {
    const unsigned char b[2]{static_cast<unsigned char>(v & 0xFF), static_cast<unsigned char>((v >> 8) & 0xFF)};
    out.write(reinterpret_cast<const char*>(b), 2);
}

void writeMinimalWav(const std::filesystem::path& path, uint16_t channels, uint32_t sampleRate,
                      uint16_t bitsPerSample, uint32_t dataSize) {
    std::ofstream out(path, std::ios::binary);
    const uint32_t byteRate = sampleRate * channels * (bitsPerSample / 8);
    const auto blockAlign = static_cast<uint16_t>(channels * (bitsPerSample / 8));
    const uint32_t riffSize = 4 + (8 + 16) + (8 + dataSize);

    out.write("RIFF", 4);
    writeU32LE(out, riffSize);
    out.write("WAVE", 4);

    out.write("fmt ", 4);
    writeU32LE(out, 16);
    writeU16LE(out, 1); // PCM
    writeU16LE(out, channels);
    writeU32LE(out, sampleRate);
    writeU32LE(out, byteRate);
    writeU16LE(out, blockAlign);
    writeU16LE(out, bitsPerSample);

    out.write("data", 4);
    writeU32LE(out, dataSize);
    const std::vector<char> zeros(dataSize, 0);
    out.write(zeros.data(), static_cast<std::streamsize>(zeros.size()));
}

} // namespace

TEST_CASE("readWavInfo parses a minimal PCM WAV header", "[WavInfo]") {
    const auto path = std::filesystem::temp_directory_path() / "sp404_core_test_minimal.wav";
    writeMinimalWav(path, 2, 44100, 16, 400);

    const auto info = sp404::readWavInfo(path);
    REQUIRE(info.has_value());
    CHECK(info->channels == 2);
    CHECK(info->sampleRate == 44100);
    CHECK(info->bitsPerSample == 16);
    CHECK(info->dataSize == 400);
    CHECK(info->dataOffset == 44); // RIFF(12) + "fmt "+size+16-byte body(24) + "data"+size(8)

    // 400 bytes / (2 channels * 2 bytes/sample) / 44100 Hz
    using Catch::Matchers::WithinAbs;
    CHECK_THAT(info->durationSeconds(), WithinAbs(100.0 / 44100.0, 1e-9));

    std::filesystem::remove(path);
}

TEST_CASE("readWavInfo returns nullopt for a non-WAV file", "[WavInfo]") {
    const auto path = std::filesystem::temp_directory_path() / "sp404_core_test_not_wav.bin";
    {
        std::ofstream out(path, std::ios::binary);
        out.write("NOTWAV..", 8);
    }

    CHECK_FALSE(sp404::readWavInfo(path).has_value());

    std::filesystem::remove(path);
}
