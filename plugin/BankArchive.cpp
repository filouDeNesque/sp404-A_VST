#include "BankArchive.h"

#include <fstream>
#include <string>
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

bool saveAllBanksToZip(const std::filesystem::path& sdRoot, const juce::File& zipDestination,
                       ProgressCallback onProgress) {
    const auto smplDirPath = smplDir(sdRoot);

    std::error_code ec;
    if (!std::filesystem::is_directory(smplDirPath, ec) || ec)
        return false;

    // Gathered into a vector first (rather than adding to the builder inline) so the total file
    // count is known upfront for progress reporting, same reasoning as syncCard.
    std::vector<juce::File> files;
    for (const auto& entry : std::filesystem::directory_iterator(smplDirPath, ec)) {
        if (ec)
            return false;
        if (entry.is_regular_file())
            files.emplace_back(entry.path().string());
    }
    if (ec)
        return false;

    juce::ZipFile::Builder builder;
    const int total = static_cast<int>(files.size());
    int current = 0;
    for (const auto& f : files) {
        builder.addFile(f, 6, f.getFileName());
        ++current;
        if (onProgress)
            onProgress(current, total, f.getFileName().toStdString());
    }

    zipDestination.deleteFile();
    std::unique_ptr<juce::FileOutputStream> out = zipDestination.createOutputStream();
    if (out == nullptr)
        return false;
    // JUCE's zip writer doesn't expose a per-file progress hook usable from outside this blocking
    // call (writeToStream's own `double*` progress parameter is designed to be polled from a
    // *different* thread while this one blocks inside it -- not worth the extra thread just for
    // this one step, see plugin/BackgroundOperation.h for where the coarser per-file progress
    // above already gets to the UI). One last step marks that compression is underway.
    if (onProgress)
        onProgress(total, total, "Compressing archive...");
    return builder.writeToStream(*out, nullptr);
}

bool loadAllBanksFromZip(const std::filesystem::path& sdRoot, const juce::File& zipFile,
                         ProgressCallback onProgress) {
    if (!zipFile.existsAsFile())
        return false;

    juce::ZipFile zip(zipFile);
    const auto tempDir = tempScratchDir("sp404_load_all");
    tempDir.createDirectory();

    const int numEntries = zip.getNumEntries();
    for (int i = 0; i < numEntries; ++i) {
        const auto* entry = zip.getEntry(i);
        if (zip.uncompressEntry(i, tempDir, true).failed()) {
            tempDir.deleteRecursively();
            return false;
        }
        if (onProgress)
            onProgress(i + 1, numEntries, entry != nullptr ? entry->filename.toStdString() : std::string());
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

bool saveBankToZip(const std::filesystem::path& sdRoot, char bankName, const juce::File& zipDestination,
                   ProgressCallback onProgress) {
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
        if (onProgress)
            onProgress(i, Bank::padCount, "Pad " + std::to_string(i));
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

bool loadBankFromZip(const std::filesystem::path& sdRoot, const juce::File& zipFile, char targetBank,
                     ProgressCallback onProgress) {
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
        if (onProgress)
            onProgress(i, Bank::padCount, "Pad " + std::to_string(i));
    }

    return true;
}

} // namespace sp404
