#pragma once

#include <juce_core/juce_core.h>

#include <filesystem>

namespace sp404 {

// Zips the whole SMPL/ folder (PAD_INFO.BIN + every sample file) verbatim -- a full mirror
// backup of the card. Returns false if the folder can't be read or the zip can't be written.
bool saveAllBanksToZip(const std::filesystem::path& sdRoot, const juce::File& zipDestination);

// Extracts to a scratch temp directory first and validates it contains a PAD_INFO.BIN of exactly
// the expected size (SdCard::totalPads * PadInfo::encodedSize) *before* touching the real SMPL/
// folder -- an invalid/corrupt zip never leaves the card partially wiped. On success, SMPL/'s
// entire current contents are deleted and replaced with the zip's.
bool loadAllBanksFromZip(const std::filesystem::path& sdRoot, const juce::File& zipFile);

// Zips one bank's PAD_INFO.BIN slice (its 12 32-byte records, see docs/sp404sx-format.md) plus
// its existing sample files, under a manifest that records which bank letter this came from --
// entries use positional names (pad01.WAV..pad12.WAV/.AIF), not the real card filenames, so
// restoring into a *different* bank letter (see loadBankFromZip) needs no filename rewriting.
bool saveBankToZip(const std::filesystem::path& sdRoot, char bankName, const juce::File& zipDestination);

struct BankZipInfo {
    bool valid = false;
    char savedFromBank = 'A';
};

// Reads just the manifest of a saveBankToZip() archive (nothing is extracted) -- used to
// pre-select the origin bank letter in the UI's bank-target picker. valid is false if zipFile
// isn't a recognizable single-bank archive (e.g. it's actually a saveAllBanksToZip() archive).
BankZipInfo peekBankZip(const juce::File& zipFile);

// Restores a saveBankToZip() archive into targetBank, which may differ from the bank it was
// originally saved from (letting the UI duplicate/rearrange banks). targetBank's 12 pads are
// fully cleared first (sp404::clearBank), so this is a full replace of that bank's slot, not a
// merge. Returns false if zipFile isn't a valid single-bank archive.
bool loadBankFromZip(const std::filesystem::path& sdRoot, const juce::File& zipFile, char targetBank);

} // namespace sp404
