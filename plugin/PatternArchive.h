#pragma once

#include <juce_core/juce_core.h>

#include <filesystem>
#include <set>

namespace sp404 {

// Zips one pattern slot's raw PTNxxxxx.BIN bytes plus every distinct pad it references (their
// PAD_INFO.BIN record and sample file), so restoring elsewhere (a different slot, a different
// card) doesn't "replay garbage" if those dependency pads aren't already present there -- see
// docs/sp404sx-format.md and the README roadmap entry this implements.
//
// Unlike BankArchive's saveBankToZip, the referenced pads keep their absolute bank+pad identity
// in the zip (entries named "<Bank><2-digit index>.PADINFO.BIN"/".WAV"/".AIF", e.g.
// "C06.PADINFO.BIN") rather than being repositioned: a pattern's raw bytes hard-code which
// bank/pad each event plays, so loading this bundle into a *different* slot doesn't change what
// it plays -- see sp404::PatternEvent::bank()/padIndexInBank(). Only restoring onto a card where
// those exact bank+pad dependencies are missing changes anything.
//
// Returns false if the slot has no pattern, or the zip can't be written.
bool savePatternToZip(const std::filesystem::path& sdRoot, char bankName, int indexInBank,
                       const juce::File& zipDestination);

struct PatternZipInfo {
    bool valid = false;
    char savedFromBank = 'A';
    int savedFromIndexInBank = 1;
    int bars = 0;
};

// Reads just the manifest of a savePatternToZip() archive (nothing is extracted) -- used to show
// the pattern's origin slot/length in the UI's load confirmation, and to reject a BankArchive zip
// dropped into "Load Pattern..." by mistake (both use manifest.json, but only this one has a
// "PATTERN.BIN" entry). valid is false if zipFile isn't a recognizable pattern archive.
PatternZipInfo peekPatternZip(const juce::File& zipFile);

struct LoadPatternResult {
    bool ok = false;
    // Every bank that had a dependency pad restored into it, plus targetBank itself -- the
    // caller (WebUIBridge) needs this to know which banks' on-screen/BankLoader state may now be
    // stale, since a pattern's dependency pads can live in a different bank than the slot it was
    // loaded into (see savePatternToZip's doc comment).
    std::set<char> touchedBanks;
};

// Restores a savePatternToZip() archive into targetBank/targetIndexInBank (the pattern's raw
// bytes are copied verbatim, so this slot doesn't need to match the slot it was originally saved
// from -- see savePatternToZip). Also restores every dependency pad bundled in the zip to its own
// original, absolute bank+pad location: a full replace of that pad (sp404::clearPad first, same
// "clobber and don't merge" philosophy as BankArchive::loadBankFromZip), not a merge -- necessary
// because the pattern's bytes reference those exact locations and can't be repointed without
// rewriting the pattern itself. ok is false if zipFile isn't a valid pattern archive, or
// targetBank/targetIndexInBank are out of range.
LoadPatternResult loadPatternFromZip(const std::filesystem::path& sdRoot, const juce::File& zipFile,
                                      char targetBank, int targetIndexInBank);

struct ExportAllPatternsResult {
    bool ok = false;
    int exportedCount = 0;
};

// Exports every occupied pattern slot (across all 10 banks x Bank::padCount pads) as a Standard
// MIDI File, bundled into a single zip -- the bulk counterpart of the per-slot "Export MIDI..."
// button (see sp404::exportPatternToMidi, plugin/PatternMidi.h). Each entry is named
// "<Bank><2-digit pad>.mid" (e.g. "A01.mid"), one per slot that actually has a pattern recorded
// -- empty slots are silently skipped, not an error. `ok` is false only if the zip itself
// couldn't be written; exportedCount == 0 (a card with no patterns at all) still leaves ok true
// (an empty zip), so the caller can tell "nothing to export" apart from "failed to write".
ExportAllPatternsResult exportAllPatternsToMidiZip(const std::filesystem::path& sdRoot,
                                                    const juce::File& zipDestination);

} // namespace sp404
