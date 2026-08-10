#include "PatternArchive.h"

#include <array>
#include <utility>

#include "PatternMidi.h"
#include "sp404/Pattern.h"
#include "sp404/SdCard.h"

namespace sp404 {

namespace {

juce::String padEntryStem(char bank, int indexInBank) {
    return juce::String::charToString(static_cast<juce::juce_wchar>(bank)) + juce::String(indexInBank).paddedLeft('0', 2);
}

juce::String padInfoEntryName(char bank, int indexInBank) {
    return padEntryStem(bank, indexInBank) + ".PADINFO.BIN";
}

juce::String sampleEntryName(char bank, int indexInBank, PadInfo::Format format) {
    return padEntryStem(bank, indexInBank) + (format == PadInfo::Format::Wave ? ".WAV" : ".AIF");
}

// Splits an entry name of the form "<Bank><2-digit index><suffix>" (e.g. "C06.PADINFO.BIN") back
// into its parts. false if name doesn't match that shape (covers manifest.json/PATTERN.BIN too,
// which callers should have already excluded, but this is defensive against any other content a
// hand-edited or foreign zip might contain).
bool parsePadEntryName(const juce::String& name, char& bank, int& indexInBank, juce::String& suffix) {
    if (name.length() < 4)
        return false;
    const auto bankChar = name[0];
    if (bankChar < 'A' || bankChar > 'J')
        return false;
    const auto digits = name.substring(1, 3);
    if (digits.length() != 2 || !digits.containsOnly("0123456789"))
        return false;
    const int index = digits.getIntValue();
    if (index < 1 || index > Bank::padCount)
        return false;

    bank = static_cast<char>(bankChar);
    indexInBank = index;
    suffix = name.substring(3);
    return true;
}

std::set<std::pair<char, int>> referencedPads(const Pattern& pattern) {
    std::set<std::pair<char, int>> pads;
    for (const auto& event : pattern.events) {
        const auto bank = event.bank();
        const auto index = event.padIndexInBank();
        if (bank && index)
            pads.insert({*bank, *index});
    }
    return pads;
}

} // namespace

bool savePatternToZip(const std::filesystem::path& sdRoot, char bankName, int indexInBank,
                       const juce::File& zipDestination) {
    if (bankName < 'A' || bankName > 'J' || indexInBank < 1 || indexInBank > Bank::padCount)
        return false;

    const juce::File patternFile(patternSlotPath(sdRoot, bankName, indexInBank).string());
    if (!patternFile.existsAsFile())
        return false;

    const auto pattern = readPattern(patternFile.getFullPathName().toStdString());
    if (!pattern)
        return false;

    juce::DynamicObject::Ptr manifest = new juce::DynamicObject();
    manifest->setProperty("savedFromBank", juce::String::charToString(static_cast<juce::juce_wchar>(bankName)));
    manifest->setProperty("savedFromIndexInBank", indexInBank);
    manifest->setProperty("bars", pattern->bars);
    manifest->setProperty("timeSignature", pattern->timeSignature);
    manifest->setProperty("formatVersion", 1);
    const auto manifestJson = juce::JSON::toString(juce::var(manifest.get()));

    juce::ZipFile::Builder builder;
    builder.addEntry(
        std::make_unique<juce::MemoryInputStream>(manifestJson.toRawUTF8(), manifestJson.getNumBytesAsUTF8(), true),
        6, "manifest.json", juce::Time::getCurrentTime());
    builder.addFile(patternFile, 6, "PATTERN.BIN");

    try {
        const SdCard card = SdCard::load(sdRoot);
        for (const auto& [padBank, padIndex] : referencedPads(*pattern)) {
            for (const auto& bank : card.banks()) {
                if (bank.name != padBank)
                    continue;
                const auto& pad = bank.pads[static_cast<size_t>(padIndex - 1)];

                std::array<std::byte, PadInfo::encodedSize> encoded{};
                pad.info.encode(encoded.data(), encoded.size());
                builder.addEntry(std::make_unique<juce::MemoryInputStream>(encoded.data(), encoded.size(), true), 6,
                                  padInfoEntryName(padBank, padIndex), juce::Time::getCurrentTime());

                if (pad.samplePath)
                    builder.addFile(juce::File(pad.samplePath->string()), 6,
                                     sampleEntryName(padBank, padIndex, pad.info.format));
                break;
            }
        }
    } catch (const std::exception&) {
        return false;
    }

    zipDestination.deleteFile();
    std::unique_ptr<juce::FileOutputStream> out = zipDestination.createOutputStream();
    if (out == nullptr)
        return false;
    return builder.writeToStream(*out, nullptr);
}

PatternZipInfo peekPatternZip(const juce::File& zipFile) {
    PatternZipInfo info;
    if (!zipFile.existsAsFile())
        return info;

    juce::ZipFile zip(zipFile);
    const int manifestIndex = zip.getIndexOfFileName("manifest.json");
    if (manifestIndex < 0 || zip.getIndexOfFileName("PATTERN.BIN") < 0)
        return info;

    std::unique_ptr<juce::InputStream> stream(zip.createStreamForEntry(manifestIndex));
    if (stream == nullptr)
        return info;

    const auto parsed = juce::JSON::parse(stream->readEntireStreamAsString());
    const auto bankStr = parsed.getProperty("savedFromBank", juce::var()).toString();
    if (bankStr.length() != 1)
        return info;

    info.valid = true;
    info.savedFromBank = static_cast<char>(bankStr[0]);
    info.savedFromIndexInBank = static_cast<int>(parsed.getProperty("savedFromIndexInBank", 1));
    info.bars = static_cast<int>(parsed.getProperty("bars", 0));
    return info;
}

LoadPatternResult loadPatternFromZip(const std::filesystem::path& sdRoot, const juce::File& zipFile, char targetBank,
                                      int targetIndexInBank) {
    LoadPatternResult result;
    if (targetBank < 'A' || targetBank > 'J' || targetIndexInBank < 1 || targetIndexInBank > Bank::padCount)
        return result;

    const auto zipInfo = peekPatternZip(zipFile);
    if (!zipInfo.valid)
        return result;

    juce::ZipFile zip(zipFile);
    const int patternIndex = zip.getIndexOfFileName("PATTERN.BIN");
    if (patternIndex < 0)
        return result;

    std::unique_ptr<juce::InputStream> patternStream(zip.createStreamForEntry(patternIndex));
    if (patternStream == nullptr)
        return result;

    const juce::File targetPatternFile(patternSlotPath(sdRoot, targetBank, targetIndexInBank).string());
    targetPatternFile.getParentDirectory().createDirectory();
    targetPatternFile.deleteFile();
    {
        std::unique_ptr<juce::FileOutputStream> patternOut = targetPatternFile.createOutputStream();
        if (patternOut == nullptr || !patternOut->writeFromInputStream(*patternStream, -1))
            return result;
    }

    // Collected first so each dependency pad is cleared exactly once before its restored content
    // is written -- matters if the bundle's sample changed extension (.AIF -> .WAV) since
    // clearPad removes both.
    std::set<std::pair<char, int>> dependencyPads;
    for (int i = 0; i < zip.getNumEntries(); ++i) {
        const auto* entry = zip.getEntry(i);
        char padBank = 'A';
        int padIndex = 1;
        juce::String suffix;
        if (entry != nullptr && parsePadEntryName(entry->filename, padBank, padIndex, suffix))
            dependencyPads.insert({padBank, padIndex});
    }
    for (const auto& [padBank, padIndex] : dependencyPads) {
        clearPad(sdRoot, padBank, padIndex);
        result.touchedBanks.insert(padBank);
    }

    for (int i = 0; i < zip.getNumEntries(); ++i) {
        const auto* entry = zip.getEntry(i);
        if (entry == nullptr)
            continue;
        char padBank = 'A';
        int padIndex = 1;
        juce::String suffix;
        if (!parsePadEntryName(entry->filename, padBank, padIndex, suffix))
            continue;

        std::unique_ptr<juce::InputStream> entryStream(zip.createStreamForEntry(i));
        if (entryStream == nullptr)
            continue;

        if (suffix == ".PADINFO.BIN") {
            juce::MemoryBlock block;
            entryStream->readIntoMemoryBlock(block);
            if (block.getSize() != PadInfo::encodedSize)
                continue;
            const auto info =
                PadInfo::decode(static_cast<const std::byte*>(block.getData()), static_cast<size_t>(block.getSize()));
            savePadInfo(sdRoot, padBank, padIndex, info);
        } else if (suffix == ".WAV" || suffix == ".AIF") {
            const auto format = suffix == ".WAV" ? PadInfo::Format::Wave : PadInfo::Format::Aiff;
            const juce::File destFile(samplePath(sdRoot, padBank, padIndex, format).string());
            std::unique_ptr<juce::FileOutputStream> destStream = destFile.createOutputStream();
            if (destStream == nullptr)
                continue;
            destStream->writeFromInputStream(*entryStream, -1);
        }
    }

    result.touchedBanks.insert(targetBank);
    result.ok = true;
    return result;
}

ExportAllPatternsResult exportAllPatternsToMidiZip(const std::filesystem::path& sdRoot,
                                                    const juce::File& zipDestination) {
    ExportAllPatternsResult result;

    // MIDI files are written here (exportPatternToMidi only knows how to write to a real file,
    // there's no in-memory variant) and added to the zip by path -- juce::ZipFile::Builder reads
    // from them lazily at writeToStream() time below, so they must stay on disk until then.
    // Cleaned up unconditionally afterwards, success or failure.
    const juce::File tempDir =
        juce::File::getSpecialLocation(juce::File::tempDirectory)
            .getChildFile("sp404_pattern_export_" + juce::String::toHexString(juce::Random::getSystemRandom().nextInt64()));
    tempDir.createDirectory();

    juce::ZipFile::Builder builder;
    for (char bank = 'A'; bank <= 'J'; ++bank) {
        for (int indexInBank = 1; indexInBank <= Bank::padCount; ++indexInBank) {
            const auto pattern = readPattern(patternSlotPath(sdRoot, bank, indexInBank));
            if (!pattern)
                continue;

            const juce::String entryName = padEntryStem(bank, indexInBank) + ".mid";
            const juce::File tempFile = tempDir.getChildFile(entryName);
            if (!exportPatternToMidi(*pattern, tempFile))
                continue;

            builder.addFile(tempFile, 6, entryName);
            ++result.exportedCount;
        }
    }

    zipDestination.deleteFile();
    {
        std::unique_ptr<juce::FileOutputStream> out = zipDestination.createOutputStream();
        result.ok = out != nullptr && builder.writeToStream(*out, nullptr);
    }

    tempDir.deleteRecursively();
    return result;
}

LoadAllPatternsResult loadAllPatternsFromMidiZip(const std::filesystem::path& sdRoot, const juce::File& zipFile) {
    LoadAllPatternsResult result;
    if (!zipFile.existsAsFile())
        return result;

    juce::ZipFile zip(zipFile);

    struct MatchedEntry {
        int entryIndex;
        char bank;
        int indexInBank;
    };
    std::vector<MatchedEntry> matches;
    for (int i = 0; i < zip.getNumEntries(); ++i) {
        const auto* entry = zip.getEntry(i);
        char bank = 'A';
        int indexInBank = 1;
        juce::String suffix;
        if (entry != nullptr && parsePadEntryName(entry->filename, bank, indexInBank, suffix) &&
            suffix.equalsIgnoreCase(".mid"))
            matches.push_back({i, bank, indexInBank});
    }

    // Validated BEFORE touching the real card -- an unrelated/empty zip must never wipe existing
    // patterns for nothing, same principle as BankArchive::loadAllBanksFromZip.
    if (matches.empty())
        return result;

    clearAllPatterns(sdRoot);

    const juce::File tempDir =
        juce::File::getSpecialLocation(juce::File::tempDirectory)
            .getChildFile("sp404_pattern_import_" + juce::String::toHexString(juce::Random::getSystemRandom().nextInt64()));
    tempDir.createDirectory();

    for (const auto& match : matches) {
        std::unique_ptr<juce::InputStream> entryStream(zip.createStreamForEntry(match.entryIndex));
        if (entryStream == nullptr)
            continue;

        const juce::File tempFile = tempDir.getChildFile(padEntryStem(match.bank, match.indexInBank) + ".mid");
        {
            std::unique_ptr<juce::FileOutputStream> out = tempFile.createOutputStream();
            if (out == nullptr || !out->writeFromInputStream(*entryStream, -1))
                continue;
        }

        if (const auto pattern = importPatternFromMidi(tempFile)) {
            writePattern(sdRoot, match.bank, match.indexInBank, *pattern);
            ++result.importedCount;
        }
    }

    tempDir.deleteRecursively();
    result.ok = true;
    return result;
}

} // namespace sp404
