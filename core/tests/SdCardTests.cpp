#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "sp404/SdCard.h"

namespace {

// Builds a synthetic card (all-zero PAD_INFO.BIN, no sample files) under a temp directory, so
// this test never touches a real SP-404SX SD card. `name` lets a test build two independent
// synthetic cards at once (e.g. a syncCard source and destination).
std::filesystem::path makeSyntheticCard(const std::string& name = "sp404_core_test_card") {
    const auto root = std::filesystem::temp_directory_path() / name;
    const auto smplDir = root / "ROLAND" / "SP-404SX" / "SMPL";
    std::filesystem::create_directories(smplDir);

    const std::vector<char> zeros(static_cast<size_t>(sp404::SdCard::totalPads) * sp404::PadInfo::encodedSize, 0);
    std::ofstream out(smplDir / "PAD_INFO.BIN", std::ios::binary);
    out.write(zeros.data(), static_cast<std::streamsize>(zeros.size()));

    return root;
}

void appendU32LE(std::vector<std::byte>& out, uint32_t v) {
    out.push_back(static_cast<std::byte>(v & 0xFF));
    out.push_back(static_cast<std::byte>((v >> 8) & 0xFF));
    out.push_back(static_cast<std::byte>((v >> 16) & 0xFF));
    out.push_back(static_cast<std::byte>((v >> 24) & 0xFF));
}

void appendU16LE(std::vector<std::byte>& out, uint16_t v) {
    out.push_back(static_cast<std::byte>(v & 0xFF));
    out.push_back(static_cast<std::byte>((v >> 8) & 0xFF));
}

void appendBytes(std::vector<std::byte>& out, const char* ascii) {
    for (; *ascii != '\0'; ++ascii)
        out.push_back(static_cast<std::byte>(*ascii));
}

// Mirrors WavInfoTests.cpp's writeMinimalWav, but builds the bytes in memory (as
// replacePadSample takes a byte buffer, not a path) rather than writing straight to a file.
std::vector<std::byte> buildMinimalWavBytes(uint16_t channels, uint32_t sampleRate, uint16_t bitsPerSample,
                                             uint32_t dataSize) {
    std::vector<std::byte> bytes;
    const uint32_t byteRate = sampleRate * channels * (bitsPerSample / 8);
    const auto blockAlign = static_cast<uint16_t>(channels * (bitsPerSample / 8));
    const uint32_t riffSize = 4 + (8 + 16) + (8 + dataSize);

    appendBytes(bytes, "RIFF");
    appendU32LE(bytes, riffSize);
    appendBytes(bytes, "WAVE");

    appendBytes(bytes, "fmt ");
    appendU32LE(bytes, 16);
    appendU16LE(bytes, 1); // PCM
    appendU16LE(bytes, channels);
    appendU32LE(bytes, sampleRate);
    appendU32LE(bytes, byteRate);
    appendU16LE(bytes, blockAlign);
    appendU16LE(bytes, bitsPerSample);

    appendBytes(bytes, "data");
    appendU32LE(bytes, dataSize);
    bytes.insert(bytes.end(), dataSize, std::byte{0});

    return bytes;
}

} // namespace

TEST_CASE("savePadInfo writes only the targeted pad's record", "[SdCard]") {
    const auto root = makeSyntheticCard();

    sp404::PadInfo edited;
    edited.volume = 100;
    edited.loop = true;
    edited.gate = true;
    edited.reverse = false;
    edited.lofi = true;

    sp404::savePadInfo(root, 'B', 5, edited);

    const auto card = sp404::SdCard::load(root);

    const auto& b5 = card.banks()[1].pads[4].info; // bank B = index 1, pad 5 = index 4
    CHECK(b5.volume == 100);
    CHECK(b5.loop == true);
    CHECK(b5.gate == true);
    CHECK(b5.reverse == false);
    CHECK(b5.lofi == true);

    // Neighbours must be untouched.
    const auto& b4 = card.banks()[1].pads[3].info;
    const auto& b6 = card.banks()[1].pads[5].info;
    const auto& a1 = card.banks()[0].pads[0].info;
    CHECK(b4.volume == 0);
    CHECK(b6.volume == 0);
    CHECK(a1.volume == 0);

    std::filesystem::remove_all(root);
}

TEST_CASE("savePadInfo rejects an out-of-range bank or pad index", "[SdCard]") {
    const auto root = makeSyntheticCard();
    const sp404::PadInfo info;

    CHECK_THROWS_AS(sp404::savePadInfo(root, 'Z', 1, info), std::invalid_argument);
    CHECK_THROWS_AS(sp404::savePadInfo(root, 'A', 0, info), std::invalid_argument);
    CHECK_THROWS_AS(sp404::savePadInfo(root, 'A', 13, info), std::invalid_argument);

    std::filesystem::remove_all(root);
}

TEST_CASE("patternSlotPath matches real PTNxxxxx.BIN filenames from a physical SP-404SX card",
          "[SdCard]") {
    const auto root = makeSyntheticCard();

    // A1, A9, A12 -- the 3 real pattern files this addressing scheme was verified against (see
    // docs/sp404sx-format.md).
    CHECK(sp404::patternSlotPath(root, 'A', 1).filename() == "PTN00001.BIN");
    CHECK(sp404::patternSlotPath(root, 'A', 9).filename() == "PTN00009.BIN");
    CHECK(sp404::patternSlotPath(root, 'A', 12).filename() == "PTN00012.BIN");

    // Unverified beyond bank A (see the HYPOTHESIS note in SdCard.h), but this is the formula
    // both community sources agree on: sequential across banks, 12 slots/bank.
    CHECK(sp404::patternSlotPath(root, 'B', 1).filename() == "PTN00013.BIN");
    CHECK(sp404::patternSlotPath(root, 'J', 12).filename() == "PTN00120.BIN");

    CHECK(sp404::patternSlotPath(root, 'A', 1).parent_path() == sp404::patternDir(root));

    std::filesystem::remove_all(root);
}

TEST_CASE("patternSlotPath rejects an out-of-range bank or pad index", "[SdCard]") {
    const auto root = makeSyntheticCard();

    CHECK_THROWS_AS(sp404::patternSlotPath(root, 'Z', 1), std::invalid_argument);
    CHECK_THROWS_AS(sp404::patternSlotPath(root, 'A', 0), std::invalid_argument);
    CHECK_THROWS_AS(sp404::patternSlotPath(root, 'A', 13), std::invalid_argument);

    std::filesystem::remove_all(root);
}

TEST_CASE("clearPatternSlot deletes the PTNxxxxx.BIN file, and is a no-op if already absent", "[SdCard]") {
    const auto root = makeSyntheticCard();
    const auto path = sp404::patternSlotPath(root, 'A', 1);
    std::filesystem::create_directories(path.parent_path());
    { std::ofstream out(path, std::ios::binary); out.write("xyz", 3); }
    REQUIRE(std::filesystem::exists(path));

    sp404::clearPatternSlot(root, 'A', 1);
    CHECK_FALSE(std::filesystem::exists(path));

    sp404::clearPatternSlot(root, 'A', 1); // already gone -- must not throw

    CHECK_THROWS_AS(sp404::clearPatternSlot(root, 'A', 13), std::invalid_argument);

    std::filesystem::remove_all(root);
}

TEST_CASE("clearAllPatterns deletes every PTNxxxxx.BIN file on the card", "[SdCard]") {
    const auto root = makeSyntheticCard();

    const std::vector<std::pair<char, int>> occupied = {{'A', 1}, {'C', 6}, {'J', 12}};
    for (const auto& [bank, indexInBank] : occupied) {
        const auto path = sp404::patternSlotPath(root, bank, indexInBank);
        std::filesystem::create_directories(path.parent_path());
        std::ofstream out(path, std::ios::binary);
        out.write("pattern", 7);
    }
    for (const auto& [bank, indexInBank] : occupied)
        REQUIRE(std::filesystem::exists(sp404::patternSlotPath(root, bank, indexInBank)));

    sp404::clearAllPatterns(root);

    for (const auto& [bank, indexInBank] : occupied)
        CHECK_FALSE(std::filesystem::exists(sp404::patternSlotPath(root, bank, indexInBank)));

    std::filesystem::remove_all(root);
}

TEST_CASE("copyPatternSlot copies raw bytes onto another slot, overwriting what was there",
          "[SdCard]") {
    const auto root = makeSyntheticCard();
    const auto srcPath = sp404::patternSlotPath(root, 'A', 1);
    std::filesystem::create_directories(srcPath.parent_path());
    { std::ofstream out(srcPath, std::ios::binary); out.write("hello-pattern", 13); }

    const auto destPath = sp404::patternSlotPath(root, 'B', 5);
    { std::ofstream out(destPath, std::ios::binary); out.write("stale-content", 13); }

    CHECK(sp404::copyPatternSlot(root, 'A', 1, 'B', 5));

    std::ifstream in(destPath, std::ios::binary);
    std::string contents((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    CHECK(contents == "hello-pattern");
    CHECK(std::filesystem::exists(srcPath)); // source is untouched by a copy

    std::filesystem::remove_all(root);
}

TEST_CASE("copyPatternSlot returns false if the source slot has no pattern", "[SdCard]") {
    const auto root = makeSyntheticCard();
    CHECK_FALSE(sp404::copyPatternSlot(root, 'A', 1, 'B', 5));
    std::filesystem::remove_all(root);
}

TEST_CASE("copyPatternSlot onto itself is a no-op that still succeeds", "[SdCard]") {
    const auto root = makeSyntheticCard();
    const auto path = sp404::patternSlotPath(root, 'A', 1);
    std::filesystem::create_directories(path.parent_path());
    { std::ofstream out(path, std::ios::binary); out.write("same-slot", 9); }

    CHECK(sp404::copyPatternSlot(root, 'A', 1, 'A', 1));

    std::ifstream in(path, std::ios::binary);
    std::string contents((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    CHECK(contents == "same-slot");

    std::filesystem::remove_all(root);
}

TEST_CASE("copyPatternSlot rejects an out-of-range bank or pad index", "[SdCard]") {
    const auto root = makeSyntheticCard();

    CHECK_THROWS_AS(sp404::copyPatternSlot(root, 'Z', 1, 'A', 1), std::invalid_argument);
    CHECK_THROWS_AS(sp404::copyPatternSlot(root, 'A', 1, 'A', 13), std::invalid_argument);

    std::filesystem::remove_all(root);
}

TEST_CASE("replacePadSample writes the WAV file and updates PAD_INFO.BIN", "[SdCard]") {
    const auto root = makeSyntheticCard();

    // Pre-existing playback settings for this pad -- should survive the swap untouched.
    sp404::PadInfo before;
    before.volume = 90;
    before.loop = true;
    before.gate = true;
    before.reverse = true;
    before.lofi = true;
    before.origSampleStart = 999; // stale values from a previous, different sample
    before.origSampleEnd = 12345;
    before.tempoMode = sp404::PadInfo::TempoMode::User;
    before.origTempo = 1200;
    before.userTempo = 1200;
    sp404::savePadInfo(root, 'C', 5, before);

    const auto wavBytes = buildMinimalWavBytes(2, 44100, 16, 400);
    sp404::replacePadSample(root, 'C', 5, wavBytes);

    const auto wavPath = sp404::samplePath(root, 'C', 5, sp404::PadInfo::Format::Wave);
    REQUIRE(std::filesystem::exists(wavPath));
    CHECK(std::filesystem::file_size(wavPath) == wavBytes.size());

    const auto card = sp404::SdCard::load(root);
    const auto& pad = card.banks()[2].pads[4].info; // bank C = index 2, pad 5 = index 4

    // Preserved playback settings.
    CHECK(pad.volume == 90);
    CHECK(pad.loop == true);
    CHECK(pad.gate == true);
    CHECK(pad.reverse == true);
    CHECK(pad.lofi == true);

    // Recomputed from the new file.
    CHECK(pad.format == sp404::PadInfo::Format::Wave);
    CHECK(pad.channels == 2);
    CHECK(pad.origSampleStart == 44); // see WavInfoTests.cpp for the minimal-header byte count
    CHECK(pad.origSampleEnd == 444);  // 44 + 400 bytes of data
    CHECK(pad.userSampleStart == 44);
    CHECK(pad.userSampleEnd == 444);
    CHECK(pad.tempoMode == sp404::PadInfo::TempoMode::Off);
    CHECK(pad.origTempo == 0);
    CHECK(pad.userTempo == 0);

    // Other pads must be untouched.
    const auto& c4 = card.banks()[2].pads[3].info;
    CHECK(c4.volume == 0);

    std::filesystem::remove_all(root);
}

TEST_CASE("replacePadSample removes a stale sample file in the other extension", "[SdCard]") {
    const auto root = makeSyntheticCard();

    const auto aifPath = sp404::samplePath(root, 'A', 1, sp404::PadInfo::Format::Aiff);
    {
        std::ofstream out(aifPath, std::ios::binary);
        out << "fake aiff data";
    }
    REQUIRE(std::filesystem::exists(aifPath));

    sp404::replacePadSample(root, 'A', 1, buildMinimalWavBytes(1, 44100, 16, 100));

    CHECK_FALSE(std::filesystem::exists(aifPath));
    CHECK(std::filesystem::exists(sp404::samplePath(root, 'A', 1, sp404::PadInfo::Format::Wave)));

    std::filesystem::remove_all(root);
}

TEST_CASE("replacePadSample rejects an out-of-range bank or pad index", "[SdCard]") {
    const auto root = makeSyntheticCard();
    const auto wavBytes = buildMinimalWavBytes(1, 44100, 16, 100);

    CHECK_THROWS_AS(sp404::replacePadSample(root, 'Z', 1, wavBytes), std::invalid_argument);
    CHECK_THROWS_AS(sp404::replacePadSample(root, 'A', 0, wavBytes), std::invalid_argument);
    CHECK_THROWS_AS(sp404::replacePadSample(root, 'A', 13, wavBytes), std::invalid_argument);

    std::filesystem::remove_all(root);
}

TEST_CASE("replacePadSample rejects data that isn't a valid WAV file", "[SdCard]") {
    const auto root = makeSyntheticCard();
    const std::vector<std::byte> garbage{std::byte{'N'}, std::byte{'O'}, std::byte{'P'}, std::byte{'E'}};

    CHECK_THROWS_AS(sp404::replacePadSample(root, 'A', 1, garbage), std::runtime_error);

    std::filesystem::remove_all(root);
}

TEST_CASE("clearPad deletes the sample file and zeroes the PadInfo record", "[SdCard]") {
    const auto root = makeSyntheticCard();
    sp404::replacePadSample(root, 'D', 3, buildMinimalWavBytes(2, 44100, 16, 200));
    const auto wavPath = sp404::samplePath(root, 'D', 3, sp404::PadInfo::Format::Wave);
    REQUIRE(std::filesystem::exists(wavPath));

    sp404::clearPad(root, 'D', 3);

    CHECK_FALSE(std::filesystem::exists(wavPath));
    const auto card = sp404::SdCard::load(root);
    const auto& cleared = card.banks()[3].pads[2]; // bank D = index 3, pad 3 = index 2
    CHECK_FALSE(cleared.samplePath.has_value());
    CHECK(cleared.info.volume == 0);
    CHECK(cleared.info.channels == 0);
    CHECK(cleared.info.origSampleEnd == 0);

    std::filesystem::remove_all(root);
}

TEST_CASE("clearPad rejects an out-of-range bank or pad index", "[SdCard]") {
    const auto root = makeSyntheticCard();

    CHECK_THROWS_AS(sp404::clearPad(root, 'Z', 1), std::invalid_argument);
    CHECK_THROWS_AS(sp404::clearPad(root, 'A', 0), std::invalid_argument);
    CHECK_THROWS_AS(sp404::clearPad(root, 'A', 13), std::invalid_argument);

    std::filesystem::remove_all(root);
}

TEST_CASE("clearBank clears every pad in a bank and leaves other banks untouched", "[SdCard]") {
    const auto root = makeSyntheticCard();
    for (int i = 1; i <= 12; ++i)
        sp404::replacePadSample(root, 'B', i, buildMinimalWavBytes(1, 44100, 16, 100));
    sp404::replacePadSample(root, 'C', 1, buildMinimalWavBytes(1, 44100, 16, 100));

    sp404::clearBank(root, 'B');

    const auto card = sp404::SdCard::load(root);
    for (int i = 0; i < 12; ++i)
        CHECK_FALSE(card.banks()[1].pads[static_cast<size_t>(i)].samplePath.has_value());
    CHECK(card.banks()[2].pads[0].samplePath.has_value()); // bank C untouched

    std::filesystem::remove_all(root);
}

TEST_CASE("clearAllBanks clears every pad on the card", "[SdCard]") {
    const auto root = makeSyntheticCard();
    sp404::replacePadSample(root, 'A', 1, buildMinimalWavBytes(1, 44100, 16, 100));
    sp404::replacePadSample(root, 'J', 12, buildMinimalWavBytes(1, 44100, 16, 100));

    sp404::clearAllBanks(root);

    const auto card = sp404::SdCard::load(root);
    for (const auto& bank : card.banks())
        for (const auto& pad : bank.pads)
            CHECK_FALSE(pad.samplePath.has_value());

    std::filesystem::remove_all(root);
}

TEST_CASE("syncCard copies source's SMPL/ contents onto destination, replacing what was there", "[SdCard]") {
    const auto source = makeSyntheticCard("sp404_core_test_sync_src");
    sp404::replacePadSample(source, 'A', 1, buildMinimalWavBytes(2, 44100, 16, 200));
    sp404::PadInfo infoA1;
    infoA1.volume = 55;
    infoA1.loop = true;
    sp404::savePadInfo(source, 'A', 1, infoA1);

    const auto dest = makeSyntheticCard("sp404_core_test_sync_dst");
    sp404::replacePadSample(dest, 'C', 1, buildMinimalWavBytes(1, 44100, 16, 50)); // must be wiped

    sp404::syncCard(source, dest);

    const auto card = sp404::SdCard::load(dest);
    CHECK(card.banks()[0].pads[0].samplePath.has_value());
    CHECK(card.banks()[0].pads[0].info.volume == 55);
    CHECK(card.banks()[0].pads[0].info.loop == true);
    CHECK_FALSE(card.banks()[2].pads[0].samplePath.has_value()); // dest's own C1 is gone

    std::filesystem::remove_all(source);
    std::filesystem::remove_all(dest);
}

TEST_CASE("syncCard rejects a source without a valid PAD_INFO.BIN and leaves destination untouched", "[SdCard]") {
    const auto source = std::filesystem::temp_directory_path() / "sp404_core_test_sync_bad_src";
    std::filesystem::remove_all(source);
    std::filesystem::create_directories(sp404::smplDir(source)); // no PAD_INFO.BIN written

    const auto dest = makeSyntheticCard("sp404_core_test_sync_dst2");
    sp404::replacePadSample(dest, 'C', 1, buildMinimalWavBytes(1, 44100, 16, 50));

    CHECK_THROWS_AS(sp404::syncCard(source, dest), std::runtime_error);

    const auto card = sp404::SdCard::load(dest);
    CHECK(card.banks()[2].pads[0].samplePath.has_value()); // untouched

    std::filesystem::remove_all(source);
    std::filesystem::remove_all(dest);
}
