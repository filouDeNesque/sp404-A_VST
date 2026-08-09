#include "sp404/SdCard.h"

#include <array>
#include <cstddef>
#include <fstream>
#include <iomanip>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <system_error>
#include <vector>

#include "sp404/WavInfo.h"

namespace sp404 {

namespace {

void checkPadRange(char bankName, int indexInBank, const char* fnName) {
    if (bankName < 'A' || bankName > 'J' || indexInBank < 1 || indexInBank > Bank::padCount)
        throw std::invalid_argument(std::string(fnName) + ": bank/pad out of range");
}

// Per-bank pad number, e.g. bank 'B' pad 3 -> "B0000003" (see docs/sp404sx-format.md).
std::string sampleStem(char bank, int indexInBank) {
    std::ostringstream oss;
    oss << bank << std::setw(7) << std::setfill('0') << indexInBank;
    return oss.str();
}

std::optional<std::filesystem::path> findSampleFile(const std::filesystem::path& smplDir,
                                                      char bank, int indexInBank) {
    const auto stem = sampleStem(bank, indexInBank);
    for (const char* ext : {".WAV", ".AIF"}) {
        auto candidate = smplDir / (stem + ext);
        if (std::filesystem::exists(candidate))
            return candidate;
    }
    return std::nullopt;
}

} // namespace

std::filesystem::path smplDir(const std::filesystem::path& sdRoot) {
    return sdRoot / "ROLAND" / "SP-404SX" / "SMPL";
}

SdCard SdCard::load(const std::filesystem::path& sdRoot) {
    const auto smplDirPath = smplDir(sdRoot);
    const auto padInfoPath = smplDirPath / "PAD_INFO.BIN";

    std::ifstream file(padInfoPath, std::ios::binary);
    if (!file)
        throw std::runtime_error("Cannot open PAD_INFO.BIN at " + padInfoPath.string());

    std::vector<char> raw((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    const size_t expectedSize = static_cast<size_t>(totalPads) * PadInfo::encodedSize;
    if (raw.size() != expectedSize) {
        throw std::runtime_error("PAD_INFO.BIN has unexpected size: expected " +
                                  std::to_string(expectedSize) + " bytes, got " +
                                  std::to_string(raw.size()));
    }

    const auto* bytes = reinterpret_cast<const std::byte*>(raw.data());

    SdCard card;
    for (int bankIndex = 0; bankIndex < numBanks; ++bankIndex) {
        const char bankName = static_cast<char>('A' + bankIndex);
        card.banks_[bankIndex].name = bankName;

        for (int padIdx = 1; padIdx <= Bank::padCount; ++padIdx) {
            const size_t recordIndex = static_cast<size_t>(bankIndex) * Bank::padCount + (padIdx - 1);
            const std::byte* record = bytes + recordIndex * PadInfo::encodedSize;

            Pad pad;
            pad.bank = bankName;
            pad.indexInBank = padIdx;
            pad.info = PadInfo::decode(record, PadInfo::encodedSize);
            pad.samplePath = findSampleFile(smplDirPath, bankName, padIdx);

            card.banks_[bankIndex].pads[static_cast<size_t>(padIdx - 1)] = pad;
        }
    }

    return card;
}

std::optional<std::filesystem::path> findConnectedCardRoot() {
    const std::filesystem::path volumesDir = "/Volumes";

    std::error_code ec;
    if (!std::filesystem::exists(volumesDir, ec) || ec)
        return std::nullopt;

    for (const auto& entry : std::filesystem::directory_iterator(volumesDir, ec)) {
        if (ec)
            break;
        if (!entry.is_directory())
            continue;

        std::error_code existsEc;
        const auto candidate = entry.path() / "ROLAND" / "SP-404SX" / "SMPL" / "PAD_INFO.BIN";
        if (std::filesystem::exists(candidate, existsEc) && !existsEc)
            return entry.path();
    }

    return std::nullopt;
}

void savePadInfo(const std::filesystem::path& sdRoot, char bankName, int indexInBank, const PadInfo& info) {
    checkPadRange(bankName, indexInBank, "savePadInfo");

    const auto padInfoPath = smplDir(sdRoot) / "PAD_INFO.BIN";

    std::fstream file(padInfoPath, std::ios::binary | std::ios::in | std::ios::out);
    if (!file)
        throw std::runtime_error("Cannot open PAD_INFO.BIN for writing at " + padInfoPath.string());

    const int bankIndex = bankName - 'A';
    const size_t recordIndex = static_cast<size_t>(bankIndex) * Bank::padCount + (indexInBank - 1);
    const auto offset = static_cast<std::streamoff>(recordIndex * PadInfo::encodedSize);

    std::array<std::byte, PadInfo::encodedSize> bytes{};
    info.encode(bytes.data(), bytes.size());

    file.seekp(offset);
    file.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (!file)
        throw std::runtime_error("Failed writing PAD_INFO.BIN at " + padInfoPath.string());
}

std::filesystem::path samplePath(const std::filesystem::path& sdRoot, char bankName, int indexInBank,
                                  PadInfo::Format format) {
    checkPadRange(bankName, indexInBank, "samplePath");
    const char* ext = format == PadInfo::Format::Wave ? ".WAV" : ".AIF";
    return smplDir(sdRoot) / (sampleStem(bankName, indexInBank) + ext);
}

void replacePadSample(const std::filesystem::path& sdRoot, char bankName, int indexInBank,
                       const std::vector<std::byte>& wavBytes) {
    checkPadRange(bankName, indexInBank, "replacePadSample");

    const auto wavPath = samplePath(sdRoot, bankName, indexInBank, PadInfo::Format::Wave);

    // A previous sample for this pad in the other extension is now stale (we always write .WAV)
    // -- remove it so a swap doesn't leave an orphaned duplicate that findSampleFile could later
    // pick up ahead of the new one.
    const auto aifPath = samplePath(sdRoot, bankName, indexInBank, PadInfo::Format::Aiff);
    std::error_code removeEc;
    std::filesystem::remove(aifPath, removeEc);

    {
        std::ofstream out(wavPath, std::ios::binary | std::ios::trunc);
        if (!out)
            throw std::runtime_error("Cannot open " + wavPath.string() + " for writing");
        out.write(reinterpret_cast<const char*>(wavBytes.data()), static_cast<std::streamsize>(wavBytes.size()));
        if (!out)
            throw std::runtime_error("Failed writing " + wavPath.string());
    }

    const auto wavInfo = readWavInfo(wavPath);
    if (!wavInfo)
        throw std::runtime_error("Not a valid WAV file: " + wavPath.string());

    const auto padInfoPath = smplDir(sdRoot) / "PAD_INFO.BIN";
    std::ifstream padInfoFile(padInfoPath, std::ios::binary);
    if (!padInfoFile)
        throw std::runtime_error("Cannot open PAD_INFO.BIN at " + padInfoPath.string());

    const int bankIndex = bankName - 'A';
    const size_t recordIndex = static_cast<size_t>(bankIndex) * Bank::padCount + (indexInBank - 1);
    const auto offset = static_cast<std::streamoff>(recordIndex * PadInfo::encodedSize);

    std::array<std::byte, PadInfo::encodedSize> existingBytes{};
    padInfoFile.seekg(offset);
    padInfoFile.read(reinterpret_cast<char*>(existingBytes.data()), static_cast<std::streamsize>(existingBytes.size()));
    if (!padInfoFile)
        throw std::runtime_error("Failed reading PAD_INFO.BIN at " + padInfoPath.string());
    padInfoFile.close();

    PadInfo updated = PadInfo::decode(existingBytes.data(), existingBytes.size());
    // volume/loop/gate/reverse/lofi describe how to play the pad, independent of which sample
    // currently occupies it -- preserved as-is. Everything else is derived from the new file.
    updated.format = PadInfo::Format::Wave;
    updated.channels = static_cast<uint8_t>(wavInfo->channels);
    updated.origSampleStart = wavInfo->dataOffset;
    updated.origSampleEnd = wavInfo->dataOffset + wavInfo->dataSize;
    updated.userSampleStart = updated.origSampleStart;
    updated.userSampleEnd = updated.origSampleEnd;
    updated.tempoMode = PadInfo::TempoMode::Off;
    updated.origTempo = 0;
    updated.userTempo = 0;

    savePadInfo(sdRoot, bankName, indexInBank, updated);
}

void clearPad(const std::filesystem::path& sdRoot, char bankName, int indexInBank) {
    checkPadRange(bankName, indexInBank, "clearPad");

    std::error_code removeEc;
    std::filesystem::remove(samplePath(sdRoot, bankName, indexInBank, PadInfo::Format::Wave), removeEc);
    std::filesystem::remove(samplePath(sdRoot, bankName, indexInBank, PadInfo::Format::Aiff), removeEc);

    // All-zero bytes on disk (not a default-constructed PadInfo{}, which encodes format=Wave/
    // channels=1 rather than all zero -- see makeSyntheticCard() in SdCardTests.cpp, which
    // already treats an all-zero record as "no sample" for this codebase's tests).
    PadInfo cleared;
    cleared.format = PadInfo::Format::Aiff; // encodes as 0
    cleared.channels = 0;
    savePadInfo(sdRoot, bankName, indexInBank, cleared);
}

void clearBank(const std::filesystem::path& sdRoot, char bankName) {
    if (bankName < 'A' || bankName > 'J')
        throw std::invalid_argument("clearBank: bank out of range");

    for (int indexInBank = 1; indexInBank <= Bank::padCount; ++indexInBank)
        clearPad(sdRoot, bankName, indexInBank);
}

void clearAllBanks(const std::filesystem::path& sdRoot) {
    for (char bankName = 'A'; bankName <= 'J'; ++bankName)
        clearBank(sdRoot, bankName);
}

void syncCard(const std::filesystem::path& sourceRoot, const std::filesystem::path& destRoot) {
    const auto sourcePadInfo = smplDir(sourceRoot) / "PAD_INFO.BIN";
    std::error_code sizeEc;
    const auto size = std::filesystem::file_size(sourcePadInfo, sizeEc);
    const auto expectedSize = static_cast<uintmax_t>(SdCard::totalPads) * PadInfo::encodedSize;
    if (sizeEc || size != expectedSize)
        throw std::runtime_error("syncCard: source has no valid PAD_INFO.BIN at " + sourcePadInfo.string());

    // Validate BEFORE touching destRoot -- an invalid source must never leave destRoot wiped.
    const auto destSmpl = smplDir(destRoot);
    std::filesystem::remove_all(destSmpl);
    std::filesystem::create_directories(destSmpl);

    std::error_code copyEc;
    std::filesystem::copy(smplDir(sourceRoot), destSmpl,
                           std::filesystem::copy_options::recursive | std::filesystem::copy_options::overwrite_existing,
                           copyEc);
    if (copyEc)
        throw std::runtime_error("syncCard: failed copying to " + destSmpl.string() + ": " + copyEc.message());
}

} // namespace sp404
