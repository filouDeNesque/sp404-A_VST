#include "BankArchive.h"

#include <fstream>
#include <system_error>
#include <vector>

#include "sp404/SdCard.h"

namespace sp404 {

namespace {

juce::String padEntryName(int indexInBank, PadInfo::Format format) {
    const juce::String ext = format == PadInfo::Format::Wave ? ".WAV" : ".AIF";
    return "pad" + juce::String(indexInBank).paddedLeft('0', 2) + ext;
}

juce::File tempScratchDir(const juce::String& label) {
    return juce::File::getSpecialLocation(juce::File::tempDirectory)
        .getChildFile(label + "_" + juce::String::toHexString(juce::Random::getSystemRandom().nextInt64()));
}

} // namespace

bool saveAllBanksToZip(const std::filesystem::path& sdRoot, const juce::File& zipDestination) {
    const auto smplDirPath = smplDir(sdRoot);

    std::error_code ec;
    if (!std::filesystem::is_directory(smplDirPath, ec) || ec)
        return false;

    juce::ZipFile::Builder builder;
    for (const auto& entry : std::filesystem::directory_iterator(smplDirPath, ec)) {
        if (ec)
            return false;
        if (!entry.is_regular_file())
            continue;
        const juce::File f(entry.path().string());
        builder.addFile(f, 6, f.getFileName());
    }

    zipDestination.deleteFile();
    std::unique_ptr<juce::FileOutputStream> out = zipDestination.createOutputStream();
    if (out == nullptr)
        return false;
    return builder.writeToStream(*out, nullptr);
}

bool loadAllBanksFromZip(const std::filesystem::path& sdRoot, const juce::File& zipFile) {
    if (!zipFile.existsAsFile())
        return false;

    juce::ZipFile zip(zipFile);
    const auto tempDir = tempScratchDir("sp404_load_all");
    tempDir.createDirectory();

    const auto uncompressResult = zip.uncompressTo(tempDir, true);
    if (uncompressResult.failed()) {
        tempDir.deleteRecursively();
        return false;
    }

    // Validate BEFORE touching the real SMPL/ folder -- an invalid/corrupt zip must never leave
    // the card partially wiped.
    const auto padInfoInTemp = tempDir.getChildFile("PAD_INFO.BIN");
    const auto expectedSize =
        static_cast<juce::int64>(SdCard::totalPads) * static_cast<juce::int64>(PadInfo::encodedSize);
    if (!padInfoInTemp.existsAsFile() || padInfoInTemp.getSize() != expectedSize) {
        tempDir.deleteRecursively();
        return false;
    }

    const juce::File smplDirFile(smplDir(sdRoot).string());
    smplDirFile.deleteRecursively();
    smplDirFile.createDirectory();

    for (const auto& f : tempDir.findChildFiles(juce::File::findFiles, false))
        f.moveFileTo(smplDirFile.getChildFile(f.getFileName()));

    tempDir.deleteRecursively();
    return true;
}

bool saveBankToZip(const std::filesystem::path& sdRoot, char bankName, const juce::File& zipDestination) {
    if (bankName < 'A' || bankName > 'J')
        return false;

    const auto padInfoPath = smplDir(sdRoot) / "PAD_INFO.BIN";
    std::ifstream in(padInfoPath, std::ios::binary);
    if (!in)
        return false;

    const int bankIndex = bankName - 'A';
    const auto sliceSize = static_cast<std::streamsize>(Bank::padCount) * static_cast<std::streamsize>(PadInfo::encodedSize);
    in.seekg(static_cast<std::streamoff>(bankIndex) * sliceSize);
    std::vector<char> slice(static_cast<size_t>(sliceSize));
    in.read(slice.data(), sliceSize);
    if (!in)
        return false;

    juce::DynamicObject::Ptr manifest = new juce::DynamicObject();
    manifest->setProperty("bankName", juce::String::charToString(static_cast<juce::juce_wchar>(bankName)));
    manifest->setProperty("formatVersion", 1);
    const auto manifestJson = juce::JSON::toString(juce::var(manifest.get()));

    juce::ZipFile::Builder builder;
    builder.addEntry(
        std::make_unique<juce::MemoryInputStream>(manifestJson.toRawUTF8(), manifestJson.getNumBytesAsUTF8(), true),
        6, "manifest.json", juce::Time::getCurrentTime());
    builder.addEntry(std::make_unique<juce::MemoryInputStream>(slice.data(), slice.size(), true), 6,
                      "PAD_INFO_SLICE.BIN", juce::Time::getCurrentTime());

    for (int i = 1; i <= Bank::padCount; ++i) {
        for (auto format : {PadInfo::Format::Wave, PadInfo::Format::Aiff}) {
            const auto realPath = samplePath(sdRoot, bankName, i, format);
            if (std::filesystem::exists(realPath))
                builder.addFile(juce::File(realPath.string()), 6, padEntryName(i, format));
        }
    }

    zipDestination.deleteFile();
    std::unique_ptr<juce::FileOutputStream> out = zipDestination.createOutputStream();
    if (out == nullptr)
        return false;
    return builder.writeToStream(*out, nullptr);
}

BankZipInfo peekBankZip(const juce::File& zipFile) {
    BankZipInfo info;
    if (!zipFile.existsAsFile())
        return info;

    juce::ZipFile zip(zipFile);
    const int index = zip.getIndexOfFileName("manifest.json");
    if (index < 0)
        return info;

    std::unique_ptr<juce::InputStream> stream(zip.createStreamForEntry(index));
    if (stream == nullptr)
        return info;

    const auto parsed = juce::JSON::parse(stream->readEntireStreamAsString());
    const auto bankNameStr = parsed.getProperty("bankName", juce::var()).toString();
    if (bankNameStr.length() != 1)
        return info;

    info.valid = true;
    info.savedFromBank = static_cast<char>(bankNameStr[0]);
    return info;
}

bool loadBankFromZip(const std::filesystem::path& sdRoot, const juce::File& zipFile, char targetBank) {
    if (targetBank < 'A' || targetBank > 'J')
        return false;

    const auto zipInfo = peekBankZip(zipFile);
    if (!zipInfo.valid)
        return false;

    juce::ZipFile zip(zipFile);
    const int sliceIndex = zip.getIndexOfFileName("PAD_INFO_SLICE.BIN");
    if (sliceIndex < 0)
        return false;

    std::unique_ptr<juce::InputStream> sliceStream(zip.createStreamForEntry(sliceIndex));
    if (sliceStream == nullptr)
        return false;

    juce::MemoryBlock sliceBlock;
    sliceStream->readIntoMemoryBlock(sliceBlock);
    const auto expectedSliceSize = static_cast<size_t>(Bank::padCount) * PadInfo::encodedSize;
    if (sliceBlock.getSize() != expectedSliceSize)
        return false;

    // Full replace of the target bank's 12 pads -- restored files/records land in a clean slot,
    // no leftovers from whatever used to be there (matters especially when the origin bank had
    // fewer populated pads than the target currently does).
    clearBank(sdRoot, targetBank);

    const auto* sliceBytes = static_cast<const std::byte*>(sliceBlock.getData());
    for (int i = 1; i <= Bank::padCount; ++i) {
        const std::byte* record = sliceBytes + static_cast<size_t>(i - 1) * PadInfo::encodedSize;
        savePadInfo(sdRoot, targetBank, i, PadInfo::decode(record, PadInfo::encodedSize));

        for (auto format : {PadInfo::Format::Wave, PadInfo::Format::Aiff}) {
            const int entryIndex = zip.getIndexOfFileName(padEntryName(i, format));
            if (entryIndex < 0)
                continue;

            std::unique_ptr<juce::InputStream> entryStream(zip.createStreamForEntry(entryIndex));
            if (entryStream == nullptr)
                continue;

            const juce::File destFile(samplePath(sdRoot, targetBank, i, format).string());
            std::unique_ptr<juce::FileOutputStream> destStream = destFile.createOutputStream();
            if (destStream == nullptr)
                continue;
            destStream->writeFromInputStream(*entryStream, -1);
        }
    }

    return true;
}

} // namespace sp404
