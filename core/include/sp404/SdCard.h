#pragma once

#include <array>
#include <filesystem>
#include <optional>
#include <vector>

#include "sp404/Bank.h"
#include "sp404/Progress.h"

namespace sp404 {

// The SP-404SX's own native sample rate -- verified 2026-08-09 by reading the raw "fmt " chunk
// of SMPL/A0000001.WAV on a real card (44100 Hz, stereo, 16-bit). Used as the resampling target
// when importing an external file onto a pad (see plugin/SampleImport.h), since the source may be
// at a DAW session's rate, or an arbitrary rate from an mp3/flac/mp4 -- neither is guaranteed to
// match the hardware's own rate. See docs/sp404sx-format.md.
inline constexpr int kNativeSampleRateHz = 44100;

// Reads ROLAND/SP-404SX/SMPL/PAD_INFO.BIN plus the sample files in that same SMPL/ folder from
// an SP-404SX SD card layout (<sdRoot>/ROLAND/SP-404SX/...). See docs/sp404sx-format.md.
// Path verified against a real SP-404SX SD card (see docs/sp404sx-format.md for details) --
// the community write-ups this was originally sourced from get the file name/location wrong.
class SdCard {
public:
    static constexpr int numBanks = 10; // A..J
    static constexpr int totalPads = numBanks * Bank::padCount;

    // Throws std::runtime_error if PAD_INFO.BIN is missing or has an unexpected size.
    static SdCard load(const std::filesystem::path& sdRoot);

    const std::array<Bank, numBanks>& banks() const { return banks_; }

private:
    std::array<Bank, numBanks> banks_;
};

// Scans /Volumes for a mounted SP-404SX SD card (a volume whose
// ROLAND/SP-404SX/SMPL/PAD_INFO.BIN exists), returning its root path if found. macOS only for
// now.
std::optional<std::filesystem::path> findConnectedCardRoot();

// <sdRoot>/ROLAND/SP-404SX/SMPL -- where PAD_INFO.BIN and every sample file live. Exposed
// publicly since plugin-side code (e.g. bank backup/restore) needs to enumerate/replace that
// directory's contents wholesale, not just individual pads.
std::filesystem::path smplDir(const std::filesystem::path& sdRoot);

// Writes a single pad's PadInfo back into PAD_INFO.BIN in place -- only that pad's 32-byte
// record is touched, everything else in the file is left untouched. bankName is 'A'..'J',
// indexInBank is 1..Bank::padCount. Throws std::invalid_argument if bankName/indexInBank are out
// of range, or std::runtime_error if the file can't be opened for read-write.
void savePadInfo(const std::filesystem::path& sdRoot, char bankName, int indexInBank, const PadInfo& info);

// Path a pad's sample file would have, given a format, whether or not it currently exists (see
// docs/sp404sx-format.md for the naming convention -- <bankName><7-digit indexInBank>.WAV/.AIF
// under SMPL/). Throws std::invalid_argument if bankName/indexInBank are out of range.
std::filesystem::path samplePath(const std::filesystem::path& sdRoot, char bankName, int indexInBank,
                                  PadInfo::Format format);

// 0-based pad sample index -- 0 for A1, +1 per pad within a bank, +Bank::padCount per bank letter
// (up to 119 for J12). Matches the `SampleIndex` field of a WAV file's RLND chunk (see
// sp404::RlndChunk/encodeWavWithRlndChunk in WavRlnd.h, docs/sp404sx-format.md) and
// PatternEvent::sampleIndex0to119()'s addressing -- same formula, exposed here so
// plugin/SampleImport.cpp doesn't need to duplicate it. Throws std::invalid_argument if
// bankName/indexInBank are out of range.
int padSampleIndex(char bankName, int indexInBank);

// <sdRoot>/ROLAND/SP-404SX/PTN -- where every PTNxxxxx.BIN pattern file lives. See
// docs/sp404sx-format.md's "PTN/PTNxxxxx.BIN" section.
std::filesystem::path patternDir(const std::filesystem::path& sdRoot);

// Path a pattern "slot" would have on disk, given the bank/pad that triggers it (the SP-404SX's
// pattern pads reuse the sample pad grid, not a separate numbering) -- whether or not a pattern
// is actually recorded there yet.
//
// HYPOTHESIS, not fully proven: sequential across all 10 banks, 12 slots/bank
// (bankIndex*Bank::padCount + indexInBank, 5-digit zero-padded decimal). Corroborated by two
// independent community sources (see docs/sp404sx-format.md) but only confirmed against real
// files for slots 1-12 (bank A) -- that's all that existed on the one real card this was checked
// against. Throws std::invalid_argument if bankName/indexInBank are out of range.
std::filesystem::path patternSlotPath(const std::filesystem::path& sdRoot, char bankName, int indexInBank);

// Deletes the PTNxxxxx.BIN file at a pattern slot, if any -- a no-op (doesn't throw or fail) if
// the slot was already empty. Throws std::invalid_argument if bankName/indexInBank are out of
// range.
void clearPatternSlot(const std::filesystem::path& sdRoot, char bankName, int indexInBank);

// clearPatternSlot for every one of the 120 possible slots (10 banks x Bank::padCount) -- resets
// every pattern on the card to empty, mirroring clearAllBanks for samples below. Slots that were
// already empty are silently skipped, same as clearPatternSlot itself.
void clearAllPatterns(const std::filesystem::path& sdRoot);

// Copies one pattern slot's raw bytes onto another slot of the same card, overwriting whatever
// was there. A pattern's bytes hard-code which bank/pad each event *plays* (see
// docs/sp404sx-format.md), not which slot it's filed under, so copying to a different slot
// doesn't change what it plays -- only which physical pattern pad triggers it (once pattern
// triggering exists). Copying a slot onto itself is a no-op that still returns true. Returns
// false if the source slot has no pattern. Throws std::invalid_argument if any bank/index is out
// of range.
bool copyPatternSlot(const std::filesystem::path& sdRoot, char srcBank, int srcIndexInBank, char destBank,
                      int destIndexInBank);

// Replaces a pad's sample with a raw WAV file (wavBytes is written verbatim, no re-encoding), and
// updates that pad's PAD_INFO.BIN record to match the new file: origSampleStart/End (byte offsets
// of the new file's "data" chunk, see docs/sp404sx-format.md) and channels/format are recomputed;
// userSampleStart/End reset to cover the whole new file (no trim yet); tempoMode reset to Off,
// origTempo/userTempo reset to a fixed non-zero fallback (kDefaultTempoTenths in SdCard.cpp, 120
// BPM) rather than 0 -- a real SP-404SX pad never has origTempo=0, even with TempoMode=Off (see
// docs/sp404sx-format.md); leaving it at 0 is believed to make the hardware's TIME/BPM tempo-match
// feature divide by zero, crashing the unit and having no audible effect (reported on real
// hardware for VST-imported pads, 2026-08-11). loop/reverse/lofi are
// always preserved from the pad's existing record. volume/gate are preserved too, *unless*
// resetPlaybackDefaults is true, in which case they're set to a fixed audible default (100/127,
// gate on) instead -- pass true for a genuinely new sample landing on a pad (drag & drop import,
// see plugin/WebUIBridge.cpp's doSwapPadSample), so it starts audible rather than silently
// inheriting volume=0 from a never-used slot (or whatever a previous, unrelated sample on that
// pad happened to be set to); pass false when rewriting a pad's *existing* sample in place (the
// DSP panel's normalize/trim/fade/etc., see plugin/SampleDsp.cpp), where resetting playback
// settings the user already tuned would be a surprising side effect of an unrelated edit. If the
// pad previously had a sample under the other extension (.AIF vs .WAV), that stale file is
// removed.
// Throws std::invalid_argument if bankName/indexInBank are out of range, or std::runtime_error if
// wavBytes isn't a valid WAV file (fmt/data chunks) once written -- note the file is still left on
// disk in that case, there is no rollback.
void replacePadSample(const std::filesystem::path& sdRoot, char bankName, int indexInBank,
                       const std::vector<std::byte>& wavBytes, bool resetPlaybackDefaults);

// Repairs pads whose sample was written before the TIME/BPM crash fix (see replacePadSample's doc
// comment above and docs/sp404sx-format.md): origTempo=userTempo=0 with a sample file present.
// Only touches such pads -- an empty pad (no sample file) legitimately has origTempo=0, same as a
// pad some other tool wrote with a deliberate origTempo=0, so this never touches a pad that isn't
// both occupied and exactly zero on both tempo fields. Sets origTempo/userTempo to the same
// non-zero fallback replacePadSample now uses (kDefaultTempoTenths, 120 BPM in SdCard.cpp);
// tempoMode/volume/gate/loop/reverse/lofi and the sample file itself are untouched -- this is a
// metadata-only repair, not a re-import. Returns the number of pads repaired (0 if none needed
// it). Throws std::runtime_error if sdRoot has no valid PAD_INFO.BIN (same as SdCard::load).
int repairZeroTempoPads(const std::filesystem::path& sdRoot);

// Deletes a pad's sample file(s) (.WAV and/or .AIF, whichever exist) and resets its PAD_INFO.BIN
// record to all-zero bytes. Note "has a sample" is determined by SdCard::load()/findSampleFile()
// purely from file existence, not from any PadInfo field -- so the file deletion is what actually
// makes the pad read back as empty; zeroing PadInfo is just hygiene (no stale trim/tempo values
// left over from whatever sample used to be there). Throws std::invalid_argument if bankName/
// indexInBank are out of range.
void clearPad(const std::filesystem::path& sdRoot, char bankName, int indexInBank);

// clearPad for every pad in a bank (1..Bank::padCount). Throws std::invalid_argument if bankName
// is out of range.
void clearBank(const std::filesystem::path& sdRoot, char bankName);

// clearBank for every bank ('A'..'J') -- resets the whole card to a blank state.
void clearAllBanks(const std::filesystem::path& sdRoot);

// Validates that sourceRoot has a correctly-sized PAD_INFO.BIN, then wholesale-replaces
// destRoot's SMPL/ contents with a copy of sourceRoot's SMPL/ (deletes whatever was in destRoot's
// SMPL/ first). Used both to seed an offline mirror from a real card and to push a mirror's
// changes back to a real card ("Synchroniser") -- see plugin/PluginProcessor.h. Throws
// std::runtime_error if sourceRoot doesn't have a valid PAD_INFO.BIN (destRoot is never touched
// in that case). If onProgress is set, it's called once per file copied (current = number of
// files copied so far, including the one just finished; total = file count in sourceRoot's SMPL/,
// known upfront since files are enumerated before any copying starts; label = that file's name).
void syncCard(const std::filesystem::path& sourceRoot, const std::filesystem::path& destRoot,
              ProgressCallback onProgress = {});

} // namespace sp404
