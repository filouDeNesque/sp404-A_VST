#include "WebUIBridge.h"

#include <cmath>
#include <cstring>
#include <filesystem>
#include <system_error>

#include "BankArchive.h"
#include "BinaryData.h"
#include "PluginProcessor.h"
#include "SampleDsp.h"
#include "SampleImport.h"
#include "sp404/SdCard.h"
#include "sp404/WavInfo.h"

namespace sp404 {

namespace {

std::optional<juce::WebBrowserComponent::Resource> provideResource(const juce::String& url) {
    // web/index.html, web/styles.css, web/app.js, web/header.png, web/screen.png,
    // web/knob0.png..web/knob6.png as separate BinaryData resources (juce_add_binary_data uses
    // just the file's base name for its symbol, directory ignored, '.' replaced with '_' -- see
    // plugin/CMakeLists.txt). knob0-6 are 7 discrete rotation frames of the same physical knob
    // artwork (see docs/ note in app.js) -- swapping between them on the frontend keeps the
    // baked-in perspective/shading correct at every position, which a single frame rotated with
    // a CSS transform can't do.
    struct Entry {
        const char* path;
        const char* data;
        int size;
        const char* mimeType;
    };
    static const Entry entries[]{
        {"/", BinaryData::index_html, BinaryData::index_htmlSize, "text/html"},
        {"/index.html", BinaryData::index_html, BinaryData::index_htmlSize, "text/html"},
        {"/styles.css", BinaryData::styles_css, BinaryData::styles_cssSize, "text/css"},
        {"/themes.css", BinaryData::themes_css, BinaryData::themes_cssSize, "text/css"},
        {"/app.js", BinaryData::app_js, BinaryData::app_jsSize, "text/javascript"},
        {"/header.png", BinaryData::header_png, BinaryData::header_pngSize, "image/png"},
        {"/screen.png", BinaryData::screen_png, BinaryData::screen_pngSize, "image/png"},
        {"/knob0.png", BinaryData::knob0_png, BinaryData::knob0_pngSize, "image/png"},
        {"/knob1.png", BinaryData::knob1_png, BinaryData::knob1_pngSize, "image/png"},
        {"/knob2.png", BinaryData::knob2_png, BinaryData::knob2_pngSize, "image/png"},
        {"/knob3.png", BinaryData::knob3_png, BinaryData::knob3_pngSize, "image/png"},
        {"/knob4.png", BinaryData::knob4_png, BinaryData::knob4_pngSize, "image/png"},
        {"/knob5.png", BinaryData::knob5_png, BinaryData::knob5_pngSize, "image/png"},
        {"/knob6.png", BinaryData::knob6_png, BinaryData::knob6_pngSize, "image/png"},
        // Decorative stickers for the Kawaii/Japandi themes -- see themes.css/app.js.
        {"/jp1.png", BinaryData::jp1_png, BinaryData::jp1_pngSize, "image/png"},
        {"/jp2.png", BinaryData::jp2_png, BinaryData::jp2_pngSize, "image/png"},
        {"/jp3.png", BinaryData::jp3_png, BinaryData::jp3_pngSize, "image/png"},
        {"/jp4.png", BinaryData::jp4_png, BinaryData::jp4_pngSize, "image/png"},
        {"/jp5.png", BinaryData::jp5_png, BinaryData::jp5_pngSize, "image/png"},
        {"/jp6.png", BinaryData::jp6_png, BinaryData::jp6_pngSize, "image/png"},
        {"/jp7.png", BinaryData::jp7_png, BinaryData::jp7_pngSize, "image/png"},
        {"/jp8.png", BinaryData::jp8_png, BinaryData::jp8_pngSize, "image/png"},
        {"/jp9.png", BinaryData::jp9_png, BinaryData::jp9_pngSize, "image/png"},
        {"/jp10.png", BinaryData::jp10_png, BinaryData::jp10_pngSize, "image/png"},
        {"/jp11.png", BinaryData::jp11_png, BinaryData::jp11_pngSize, "image/png"},
        {"/jp12.png", BinaryData::jp12_png, BinaryData::jp12_pngSize, "image/png"},
        {"/jp13.png", BinaryData::jp13_png, BinaryData::jp13_pngSize, "image/png"},
        {"/kawaii1.png", BinaryData::kawaii1_png, BinaryData::kawaii1_pngSize, "image/png"},
        {"/kawaii2.png", BinaryData::kawaii2_png, BinaryData::kawaii2_pngSize, "image/png"},
        {"/kawaii3.png", BinaryData::kawaii3_png, BinaryData::kawaii3_pngSize, "image/png"},
        {"/kawaii4.png", BinaryData::kawaii4_png, BinaryData::kawaii4_pngSize, "image/png"},
        {"/kawaii5.png", BinaryData::kawaii5_png, BinaryData::kawaii5_pngSize, "image/png"},
        {"/kawaii6.png", BinaryData::kawaii6_png, BinaryData::kawaii6_pngSize, "image/png"},
        {"/kawaii7.png", BinaryData::kawaii7_png, BinaryData::kawaii7_pngSize, "image/png"},
        {"/kawaii8.png", BinaryData::kawaii8_png, BinaryData::kawaii8_pngSize, "image/png"},
        {"/kawaii9.png", BinaryData::kawaii9_png, BinaryData::kawaii9_pngSize, "image/png"},
        {"/kawaii10.png", BinaryData::kawaii10_png, BinaryData::kawaii10_pngSize, "image/png"},
        {"/kawaii11.png", BinaryData::kawaii11_png, BinaryData::kawaii11_pngSize, "image/png"},
        {"/kawaii12.png", BinaryData::kawaii12_png, BinaryData::kawaii12_pngSize, "image/png"},
    };

    for (const auto& entry : entries) {
        if (url != entry.path)
            continue;

        std::vector<std::byte> data(static_cast<size_t>(entry.size));
        std::memcpy(data.data(), entry.data, data.size());
        return juce::WebBrowserComponent::Resource{std::move(data), entry.mimeType};
    }

    return std::nullopt;
}

juce::String tempoModeToString(PadInfo::TempoMode mode) {
    switch (mode) {
        case PadInfo::TempoMode::Pattern:
            return "pattern";
        case PadInfo::TempoMode::User:
            return "user";
        case PadInfo::TempoMode::Off:
        default:
            return "off";
    }
}

juce::String formatToString(PadInfo::Format format) {
    return format == PadInfo::Format::Wave ? "wave" : "aiff";
}

// Re-scans /Volumes (or the offline mirror, see PluginProcessor::resolveCardRoot) and re-reads
// PADINFO.BIN on every call. Fine for now (small file, called on demand from the UI thread rather
// than per audio block) — worth caching if this becomes a bottleneck once the UI polls more
// aggressively.
void listBanks(PluginProcessor& processor, const juce::Array<juce::var>&,
                juce::WebBrowserComponent::NativeFunctionCompletion completion) {
    const auto cardRoot = processor.resolveCardRoot();

    auto* response = new juce::DynamicObject();
    response->setProperty("connected", cardRoot.has_value());

    // Capacity/free space of the volume the card is mounted on, for the SD icon's tooltip.
    if (cardRoot) {
        std::error_code ec;
        const auto space = std::filesystem::space(*cardRoot, ec);
        if (!ec) {
            response->setProperty("totalBytes", static_cast<juce::int64>(space.capacity));
            response->setProperty("freeBytes", static_cast<juce::int64>(space.available));
        }
    }

    juce::Array<juce::var> banks;
    for (char bankName = 'A'; bankName <= 'J'; ++bankName) {
        auto* bank = new juce::DynamicObject();
        const char nameBuf[2]{bankName, '\0'};
        bank->setProperty("name", juce::String(nameBuf));
        bank->setProperty("padCount", Bank::padCount);
        banks.add(juce::var(bank));
    }
    response->setProperty("banks", banks);

    completion(juce::var(response));
}

void listPads(PluginProcessor& processor, const juce::Array<juce::var>& args,
              juce::WebBrowserComponent::NativeFunctionCompletion completion) {
    auto* response = new juce::DynamicObject();
    const auto cardRoot = processor.resolveCardRoot();
    response->setProperty("connected", cardRoot.has_value());

    juce::Array<juce::var> padsVar;

    if (cardRoot.has_value() && args.size() > 0 && args[0].toString().length() == 1) {
        const char bankChar = static_cast<char>(args[0].toString()[0]);

        try {
            const SdCard card = SdCard::load(*cardRoot);
            for (const auto& bank : card.banks()) {
                if (bank.name != bankChar)
                    continue;

                for (const auto& pad : bank.pads) {
                    auto* padVar = new juce::DynamicObject();
                    padVar->setProperty("label", juce::String(pad.label()));
                    padVar->setProperty("sampleName",
                                         pad.samplePath
                                             ? juce::var(juce::String(pad.samplePath->filename().string().c_str()))
                                             : juce::var());
                    padVar->setProperty("volume", pad.info.volume);
                    padVar->setProperty("loop", pad.info.loop);
                    padVar->setProperty("gate", pad.info.gate);
                    padVar->setProperty("reverse", pad.info.reverse);
                    padVar->setProperty("lofi", pad.info.lofi);
                    padVar->setProperty("channels", pad.info.channels);
                    padVar->setProperty("format", formatToString(pad.info.format));
                    padVar->setProperty("tempoMode", tempoModeToString(pad.info.tempoMode));
                    padVar->setProperty("origBpm", pad.info.origTempo / 10.0);
                    padVar->setProperty("userBpm", pad.info.userTempo / 10.0);
                    padVar->setProperty("origSampleStart", static_cast<juce::int64>(pad.info.origSampleStart));
                    padVar->setProperty("origSampleEnd", static_cast<juce::int64>(pad.info.origSampleEnd));
                    padVar->setProperty("userSampleStart", static_cast<juce::int64>(pad.info.userSampleStart));
                    padVar->setProperty("userSampleEnd", static_cast<juce::int64>(pad.info.userSampleEnd));

                    // Actual audio duration read from the WAV file itself (independent of the
                    // origSampleStart/End frame offsets above). AIFF pads aren't parsed yet.
                    juce::var durationSeconds;
                    if (pad.samplePath && pad.info.format == PadInfo::Format::Wave) {
                        if (const auto wavInfo = readWavInfo(*pad.samplePath))
                            durationSeconds = wavInfo->durationSeconds();
                    }
                    padVar->setProperty("durationSeconds", durationSeconds);

                    padsVar.add(juce::var(padVar));
                }
                break;
            }
        } catch (const std::exception&) {
            // PADINFO.BIN vanished or changed shape between findConnectedCardRoot() and here
            // (e.g. card was ejected mid-call) -- report as disconnected rather than crash.
            response->setProperty("connected", false);
        }
    }

    response->setProperty("pads", padsVar);
    completion(juce::var(response));
}

// args: [bankChar, indexInBank (1-12), updates]. `updates` is a JS object with any subset of
// {volume, loop, gate, reverse, lofi} -- only those fields are changed, everything else in the
// pad's PadInfo (trim points, tempo, format, ...) is read back from disk first and preserved.
// Writes immediately (see README) -- there is no separate "save" step and no undo.
void updatePad(PluginProcessor& processor, const juce::Array<juce::var>& args,
               juce::WebBrowserComponent::NativeFunctionCompletion completion) {
    bool ok = false;

    if (const auto cardRoot = processor.resolveCardRoot()) {
        if (args.size() > 2 && args[0].toString().length() == 1) {
            const char bankChar = static_cast<char>(args[0].toString()[0]);
            const int indexInBank = static_cast<int>(args[1]);
            auto* updates = args[2].getDynamicObject();

            if (updates != nullptr && indexInBank >= 1 && indexInBank <= Bank::padCount) {
                try {
                    const SdCard card = SdCard::load(*cardRoot);
                    for (const auto& bank : card.banks()) {
                        if (bank.name != bankChar)
                            continue;

                        PadInfo info = bank.pads[static_cast<size_t>(indexInBank - 1)].info;

                        if (updates->hasProperty("volume"))
                            info.volume = static_cast<uint8_t>(static_cast<int>(updates->getProperty("volume")));
                        if (updates->hasProperty("loop"))
                            info.loop = static_cast<bool>(updates->getProperty("loop"));
                        if (updates->hasProperty("gate"))
                            info.gate = static_cast<bool>(updates->getProperty("gate"));
                        if (updates->hasProperty("reverse"))
                            info.reverse = static_cast<bool>(updates->getProperty("reverse"));
                        if (updates->hasProperty("lofi"))
                            info.lofi = static_cast<bool>(updates->getProperty("lofi"));

                        savePadInfo(*cardRoot, bankChar, indexInBank, info);
                        ok = true;
                        break;
                    }
                } catch (const std::exception&) {
                    ok = false;
                }
            }

            // Re-arm the bank so BankLoader re-reads it from disk and playback (MIDI + preview)
            // reflects the edit immediately, whether or not the write above succeeded.
            processor.requestBankChange(bankChar);
        }
    }

    auto* response = new juce::DynamicObject();
    response->setProperty("ok", ok);
    completion(juce::var(response));
}

// Shared by swapPadSample/swapPadSampleFromPath below: decoded is the source audio already
// decoded+resampled+re-encoded to a card-ready WAV by sp404::importAudioToWav (nullopt if the
// source couldn't be decoded at all). Replaces the pad's sample and, if that succeeds, re-arms
// the bank so BankLoader picks up the new file immediately.
bool doSwapPadSample(PluginProcessor& processor, const std::filesystem::path& cardRoot, char bankChar,
                      int indexInBank, std::optional<std::vector<std::byte>> decoded) {
    if (indexInBank < 1 || indexInBank > Bank::padCount || !decoded.has_value())
        return false;

    bool ok = false;
    try {
        replacePadSample(cardRoot, bankChar, indexInBank, *decoded);
        ok = true;
    } catch (const std::exception&) {
        ok = false;
    }

    if (ok)
        processor.requestBankChange(bankChar);
    return ok;
}

// args: [bankChar, indexInBank (1-12), base64AudioBytes]. base64AudioBytes is any audio file
// JUCE can decode (wav/aiff/flac natively, +mp3/mp4/m4a via CoreAudioFormat on macOS -- see
// sp404::importAudioToWav), dropped by the user onto a pad in the WebView (see wirePadDragDrop in
// app.js). Decoded, resampled to the SP-404SX's native rate if needed, and re-encoded before
// writing (see sp404::replacePadSample). Writes immediately, same as updatePad -- no separate
// "save" step, no undo.
void swapPadSample(PluginProcessor& processor, const juce::Array<juce::var>& args,
                    juce::WebBrowserComponent::NativeFunctionCompletion completion) {
    bool ok = false;

    if (const auto cardRoot = processor.resolveCardRoot()) {
        if (args.size() > 2 && args[0].toString().length() == 1) {
            const char bankChar = static_cast<char>(args[0].toString()[0]);
            const int indexInBank = static_cast<int>(args[1]);

            juce::MemoryOutputStream decodedBase64;
            if (juce::Base64::convertFromBase64(decodedBase64, args[2].toString())) {
                auto sourceStream = std::make_unique<juce::MemoryInputStream>(
                    decodedBase64.getData(), decodedBase64.getDataSize(), true);
                ok = doSwapPadSample(processor, *cardRoot, bankChar, indexInBank,
                                      importAudioToWav(std::move(sourceStream)));
            }
        }
    }

    auto* response = new juce::DynamicObject();
    response->setProperty("ok", ok);
    completion(juce::var(response));
}

// args: [bankChar, indexInBank (1-12), sourcePath]. Fallback for drag sources that don't put a
// real OS file into the HTML5 drag session (dataTransfer.files stays empty) but do expose a
// filesystem path as plain text -- observed with Ableton's Splice browser panel, which apparently
// only offers "text/plain" on drag, not a File (see wirePadDragDrop's extractDroppedFilePath).
// Reads sourcePath directly from disk natively, bypassing the browser entirely -- also sidesteps
// the base64/JSON-over-the-bridge transfer cost swapPadSample pays for large files.
void swapPadSampleFromPath(PluginProcessor& processor, const juce::Array<juce::var>& args,
                            juce::WebBrowserComponent::NativeFunctionCompletion completion) {
    bool ok = false;

    if (const auto cardRoot = processor.resolveCardRoot()) {
        if (args.size() > 2 && args[0].toString().length() == 1) {
            const char bankChar = static_cast<char>(args[0].toString()[0]);
            const int indexInBank = static_cast<int>(args[1]);
            const juce::File sourceFile(args[2].toString());

            if (sourceFile.existsAsFile())
                ok = doSwapPadSample(processor, *cardRoot, bankChar, indexInBank, importAudioToWav(sourceFile));
        }
    }

    auto* response = new juce::DynamicObject();
    response->setProperty("ok", ok);
    completion(juce::var(response));
}

void respondOk(bool ok, juce::WebBrowserComponent::NativeFunctionCompletion completion) {
    auto* response = new juce::DynamicObject();
    response->setProperty("ok", ok);
    completion(juce::var(response));
}

// Native file dialogs -- the bank-management menu's only use of anything outside the WebView
// (there's no browser equivalent for a real "Save As" to an arbitrary location, or for picking an
// existing file by path rather than by drag/drop -- see plan notes). Async (launchAsync), not the
// blocking browseForFileToOpen/Save, so the message thread -- and the WebView -- stay responsive
// while the panel is open. The FileChooser is kept alive by the shared_ptr captured in its own
// completion lambda; once that lambda fires and is destroyed, the chooser is too.
void handlePickZipToOpen(juce::WebBrowserComponent::NativeFunctionCompletion completion) {
    auto chooser = std::make_shared<juce::FileChooser>("Choose a .zip backup", juce::File(), "*.zip");
    chooser->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                          [chooser, completion](const juce::FileChooser& fc) {
                              const auto path = fc.getResult().getFullPathName();
                              auto* response = new juce::DynamicObject();
                              if (path.isNotEmpty())
                                  response->setProperty("path", path);
                              else
                                  response->setProperty("cancelled", true);
                              completion(juce::var(response));
                          });
}

// args: [defaultFileName]
void handlePickZipToSave(const juce::Array<juce::var>& args,
                          juce::WebBrowserComponent::NativeFunctionCompletion completion) {
    const juce::String defaultName = args.size() > 0 ? args[0].toString() : juce::String("backup.zip");
    const auto defaultFile = juce::File::getSpecialLocation(juce::File::userDocumentsDirectory).getChildFile(defaultName);

    auto chooser = std::make_shared<juce::FileChooser>("Save backup as", defaultFile, "*.zip");
    chooser->launchAsync(
        juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::warnAboutOverwriting,
        [chooser, completion](const juce::FileChooser& fc) {
            auto path = fc.getResult().getFullPathName();
            auto* response = new juce::DynamicObject();
            if (path.isNotEmpty()) {
                if (!path.endsWithIgnoreCase(".zip"))
                    path += ".zip";
                response->setProperty("path", path);
            } else {
                response->setProperty("cancelled", true);
            }
            completion(juce::var(response));
        });
}

// args: [zipPath]
void handleSaveAllBanks(PluginProcessor& processor, const juce::Array<juce::var>& args,
                        juce::WebBrowserComponent::NativeFunctionCompletion completion) {
    bool ok = false;
    if (const auto cardRoot = processor.resolveCardRoot(); cardRoot && args.size() > 0)
        ok = saveAllBanksToZip(*cardRoot, juce::File(args[0].toString()));
    respondOk(ok, completion);
}

// args: [zipPath]
void handleLoadAllBanks(PluginProcessor& processor, const juce::Array<juce::var>& args,
                        juce::WebBrowserComponent::NativeFunctionCompletion completion) {
    bool ok = false;
    if (const auto cardRoot = processor.resolveCardRoot(); cardRoot && args.size() > 0)
        ok = loadAllBanksFromZip(*cardRoot, juce::File(args[0].toString()));
    respondOk(ok, completion);
}

// args: [bankChar, zipPath]
void handleSaveBank(PluginProcessor& processor, const juce::Array<juce::var>& args,
                     juce::WebBrowserComponent::NativeFunctionCompletion completion) {
    bool ok = false;
    if (const auto cardRoot = processor.resolveCardRoot(); cardRoot && args.size() > 1 && args[0].toString().length() == 1)
        ok = saveBankToZip(*cardRoot, static_cast<char>(args[0].toString()[0]), juce::File(args[1].toString()));
    respondOk(ok, completion);
}

// args: [zipPath] -- reads a single-bank archive's manifest only, nothing is extracted. Used by
// the UI to pre-select the origin bank letter in its bank-target picker before the user confirms.
void handlePeekBankZip(const juce::Array<juce::var>& args,
                       juce::WebBrowserComponent::NativeFunctionCompletion completion) {
    auto* response = new juce::DynamicObject();
    BankZipInfo info;
    if (args.size() > 0)
        info = peekBankZip(juce::File(args[0].toString()));
    response->setProperty("valid", info.valid);
    if (info.valid) {
        const char nameBuf[2]{info.savedFromBank, '\0'};
        response->setProperty("savedFromBank", juce::String(nameBuf));
    }
    completion(juce::var(response));
}

// args: [zipPath, targetBankChar] -- targetBankChar may differ from the bank the archive was
// originally saved from (see sp404::loadBankFromZip).
void handleLoadBank(PluginProcessor& processor, const juce::Array<juce::var>& args,
                     juce::WebBrowserComponent::NativeFunctionCompletion completion) {
    bool ok = false;
    if (const auto cardRoot = processor.resolveCardRoot(); cardRoot && args.size() > 1 && args[1].toString().length() == 1)
        ok = loadBankFromZip(*cardRoot, juce::File(args[0].toString()), static_cast<char>(args[1].toString()[0]));
    respondOk(ok, completion);
}

// args: [bankChar]
void handleClearBank(PluginProcessor& processor, const juce::Array<juce::var>& args,
                      juce::WebBrowserComponent::NativeFunctionCompletion completion) {
    bool ok = false;
    if (const auto cardRoot = processor.resolveCardRoot(); cardRoot && args.size() > 0 && args[0].toString().length() == 1) {
        try {
            clearBank(*cardRoot, static_cast<char>(args[0].toString()[0]));
            ok = true;
        } catch (const std::exception&) {
            ok = false;
        }
    }
    respondOk(ok, completion);
}

void handleClearAllBanks(PluginProcessor& processor, const juce::Array<juce::var>&,
                          juce::WebBrowserComponent::NativeFunctionCompletion completion) {
    bool ok = false;
    if (const auto cardRoot = processor.resolveCardRoot()) {
        try {
            clearAllBanks(*cardRoot);
            ok = true;
        } catch (const std::exception&) {
            ok = false;
        }
    }
    respondOk(ok, completion);
}

// --- Offline/live sync mode -----------------------------------------------------------------

// Whether the mirror itself already has a valid PAD_INFO.BIN -- independent of whether offline
// mode is currently *active* (unlike PluginProcessor::resolveCardRoot(), which only returns the
// mirror when both conditions hold). The UI needs this on its own, e.g. to know upfront whether
// switching to offline mode will need a card connected first.
bool isMirrorSeeded() {
    std::error_code sizeEc;
    const auto expectedSize = static_cast<uintmax_t>(SdCard::totalPads) * PadInfo::encodedSize;
    const auto size = std::filesystem::file_size(smplDir(PluginProcessor::mirrorRoot()) / "PAD_INFO.BIN", sizeEc);
    return !sizeEc && size == expectedSize;
}

void handleGetSyncMode(PluginProcessor& processor, const juce::Array<juce::var>&,
                        juce::WebBrowserComponent::NativeFunctionCompletion completion) {
    auto* response = new juce::DynamicObject();
    response->setProperty("offline", processor.isOfflineMode());
    response->setProperty("mirrorReady", isMirrorSeeded());
    completion(juce::var(response));
}

// Seeds the offline mirror from the currently-connected real card if it isn't already seeded,
// then switches to offline mode. Requires a real card connected right now if the mirror is empty
// -- there's nothing to mirror otherwise.
void handleEnterOfflineMode(PluginProcessor& processor, const juce::Array<juce::var>&,
                             juce::WebBrowserComponent::NativeFunctionCompletion completion) {
    bool ok = false;
    const auto mirror = PluginProcessor::mirrorRoot();

    if (isMirrorSeeded()) {
        ok = true;
    } else if (const auto realRoot = findConnectedCardRoot()) {
        try {
            syncCard(*realRoot, mirror);
            ok = true;
        } catch (const std::exception&) {
            ok = false;
        }
    }

    if (ok)
        processor.setOfflineMode(true);
    respondOk(ok, completion);
}

void handleExitOfflineMode(PluginProcessor& processor, const juce::Array<juce::var>&,
                            juce::WebBrowserComponent::NativeFunctionCompletion completion) {
    processor.setOfflineMode(false); // the mirror stays on disk for next time
    respondOk(true, completion);
}

// Pushes the offline mirror's contents onto the currently-connected real card, overwriting it.
void handleSyncMirrorToCard(PluginProcessor& processor, const juce::Array<juce::var>&,
                             juce::WebBrowserComponent::NativeFunctionCompletion completion) {
    bool ok = false;
    if (processor.isOfflineMode()) {
        if (const auto realRoot = findConnectedCardRoot()) {
            try {
                syncCard(PluginProcessor::mirrorRoot(), *realRoot);
                ok = true;
            } catch (const std::exception&) {
                ok = false;
            }
        }
    }
    respondOk(ok, completion);
}

// --- Theme preference ---------------------------------------------------------------------
// A single small JSON file ({"theme": "id"}) at PluginProcessor::prefsPath() -- a UI preference
// that isn't tied to any one DAW project, so it lives in a global prefs file rather than
// getStateInformation. Not validated against the known theme list here: an unrecognized id just
// means themes.css has no matching [data-theme="..."] block, so the Hardware defaults in :root
// apply -- harmless either way, no need to duplicate app.js's theme list on the native side.

juce::var readPrefsJson() {
    const juce::File file(PluginProcessor::prefsPath().string());
    if (!file.existsAsFile())
        return {};
    return juce::JSON::parse(file.loadFileAsString());
}

void writePrefsJson(const juce::var& prefs) {
    const juce::File file(PluginProcessor::prefsPath().string());
    file.getParentDirectory().createDirectory();
    file.replaceWithText(juce::JSON::toString(prefs));
}

void handleGetTheme(const juce::Array<juce::var>&, juce::WebBrowserComponent::NativeFunctionCompletion completion) {
    const auto prefs = readPrefsJson();
    juce::String theme = "hardware";
    if (auto* obj = prefs.getDynamicObject(); obj != nullptr && obj->hasProperty("theme")) {
        const auto stored = obj->getProperty("theme").toString();
        if (stored.isNotEmpty())
            theme = stored;
    }

    auto* response = new juce::DynamicObject();
    response->setProperty("theme", theme);
    completion(juce::var(response));
}

void handleSetTheme(const juce::Array<juce::var>& args, juce::WebBrowserComponent::NativeFunctionCompletion completion) {
    bool ok = false;
    if (args.size() > 0 && args[0].toString().isNotEmpty()) {
        auto prefs = readPrefsJson();
        juce::DynamicObject::Ptr obj = prefs.getDynamicObject();
        if (obj == nullptr)
            obj = new juce::DynamicObject();
        obj->setProperty("theme", args[0].toString());
        writePrefsJson(juce::var(obj.get()));
        ok = true;
    }
    respondOk(ok, completion);
}

// --- Per-pad DSP panel -------------------------------------------------------------------------
// Each of these applies to the pad's *existing* sample (see plugin/SampleDsp.h for the shared
// "reset trim like a fresh import, preserve volume/loop/gate/reverse/lofi" contract) and writes
// immediately -- same "no save step, no undo" rule as every other pad edit in this app.

// args: [bankChar, indexInBank]. Read-only -- returns {ok, channels, isSilent, peakDb,
// leadingSilenceSeconds, trailingSilenceSeconds} (peak/silence fields omitted if isSilent). The
// DSP panel calls this on open and after every action so it can say "already normalized"/
// "already stereo"/"no silence to trim" instead of a generic "done" that looks the same whether
// or not anything actually changed -- see SampleDsp.h's PadDspStatus.
void handleGetPadDspStatus(PluginProcessor& processor, const juce::Array<juce::var>& args,
                            juce::WebBrowserComponent::NativeFunctionCompletion completion) {
    std::optional<PadDspStatus> status;
    if (const auto cardRoot = processor.resolveCardRoot(); cardRoot && args.size() > 1 && args[0].toString().length() == 1)
        status = getPadDspStatus(*cardRoot, static_cast<char>(args[0].toString()[0]), static_cast<int>(args[1]));

    auto* response = new juce::DynamicObject();
    response->setProperty("ok", status.has_value());
    if (status) {
        response->setProperty("channels", status->channels);
        response->setProperty("isSilent", status->isSilent);
        if (!status->isSilent) {
            response->setProperty("peakDb", status->peakDb);
            response->setProperty("leadingSilenceSeconds", status->leadingSilenceSeconds);
            response->setProperty("trailingSilenceSeconds", status->trailingSilenceSeconds);
        }
    }
    completion(juce::var(response));
}

// args: [bankChar, indexInBank]. Response adds peakBeforeDb/peakAfterDb (omitted if the pad was
// silent) so the UI can tell "already normalized" (before ~= after) from an actual change.
void handleNormalizePad(PluginProcessor& processor, const juce::Array<juce::var>& args,
                         juce::WebBrowserComponent::NativeFunctionCompletion completion) {
    bool ok = false;
    char bankChar = 0;
    std::optional<PadDspStatus> before, after;
    if (const auto cardRoot = processor.resolveCardRoot(); cardRoot && args.size() > 1 && args[0].toString().length() == 1) {
        bankChar = static_cast<char>(args[0].toString()[0]);
        const int indexInBank = static_cast<int>(args[1]);
        before = getPadDspStatus(*cardRoot, bankChar, indexInBank);
        ok = normalizePad(*cardRoot, bankChar, indexInBank);
        if (ok)
            after = getPadDspStatus(*cardRoot, bankChar, indexInBank);
    }
    // Re-arm the bank so BankLoader re-reads the just-modified file from disk -- without this,
    // the sample on disk is correctly changed but MIDI/preview playback keeps using the stale
    // in-memory buffer until the bank is reloaded some other way (e.g. re-clicking the bank
    // button), which looks exactly like "nothing happened". Same pattern as updatePad.
    if (ok)
        processor.requestBankChange(bankChar);

    auto* response = new juce::DynamicObject();
    response->setProperty("ok", ok);
    if (before && !before->isSilent)
        response->setProperty("peakBeforeDb", before->peakDb);
    if (after && !after->isSilent)
        response->setProperty("peakAfterDb", after->peakDb);
    completion(juce::var(response));
}

// args: [bankChar, indexInBank, targetChannels (1 or 2)]. Response adds channelsBefore so the UI
// can say "already stereo" rather than implying a conversion happened.
void handleConvertPadChannels(PluginProcessor& processor, const juce::Array<juce::var>& args,
                               juce::WebBrowserComponent::NativeFunctionCompletion completion) {
    bool ok = false;
    char bankChar = 0;
    int channelsBefore = 0;
    if (const auto cardRoot = processor.resolveCardRoot(); cardRoot && args.size() > 2 && args[0].toString().length() == 1) {
        bankChar = static_cast<char>(args[0].toString()[0]);
        const int indexInBank = static_cast<int>(args[1]);
        if (const auto before = getPadDspStatus(*cardRoot, bankChar, indexInBank))
            channelsBefore = before->channels;
        ok = convertPadChannels(*cardRoot, bankChar, indexInBank, static_cast<int>(args[2]));
    }
    if (ok)
        processor.requestBankChange(bankChar);

    auto* response = new juce::DynamicObject();
    response->setProperty("ok", ok);
    response->setProperty("channelsBefore", channelsBefore);
    completion(juce::var(response));
}

// args: [bankChar, indexInBank, fadeInSeconds, fadeOutSeconds]
void handleFadePad(PluginProcessor& processor, const juce::Array<juce::var>& args,
                    juce::WebBrowserComponent::NativeFunctionCompletion completion) {
    bool ok = false;
    char bankChar = 0;
    if (const auto cardRoot = processor.resolveCardRoot(); cardRoot && args.size() > 3 && args[0].toString().length() == 1) {
        bankChar = static_cast<char>(args[0].toString()[0]);
        ok = fadePad(*cardRoot, bankChar, static_cast<int>(args[1]), static_cast<double>(args[2]),
                      static_cast<double>(args[3]));
    }
    if (ok)
        processor.requestBankChange(bankChar);
    respondOk(ok, completion);
}

// args: [bankChar, indexInBank]. Response adds leadingSilenceSecondsBefore/
// trailingSilenceSecondsBefore (as measured right before trimming) so the UI can say "no silence
// to trim" instead of implying a cut happened when the sample was already tight.
void handleTrimSilencePad(PluginProcessor& processor, const juce::Array<juce::var>& args,
                           juce::WebBrowserComponent::NativeFunctionCompletion completion) {
    bool ok = false;
    char bankChar = 0;
    std::optional<PadDspStatus> before;
    if (const auto cardRoot = processor.resolveCardRoot(); cardRoot && args.size() > 1 && args[0].toString().length() == 1) {
        bankChar = static_cast<char>(args[0].toString()[0]);
        const int indexInBank = static_cast<int>(args[1]);
        before = getPadDspStatus(*cardRoot, bankChar, indexInBank);
        ok = trimSilencePad(*cardRoot, bankChar, indexInBank);
    }
    if (ok)
        processor.requestBankChange(bankChar);

    auto* response = new juce::DynamicObject();
    response->setProperty("ok", ok);
    response->setProperty("hadSample", before.has_value());
    response->setProperty("wasSilent", before.has_value() && before->isSilent);
    if (before && !before->isSilent) {
        response->setProperty("leadingSilenceSecondsBefore", before->leadingSilenceSeconds);
        response->setProperty("trailingSilenceSecondsBefore", before->trailingSilenceSeconds);
    }
    completion(juce::var(response));
}

// args: [bankChar, indexInBank]. Estimates BPM (read-only w.r.t. the sample itself) and, on
// success, saves it into the pad's userTempo/tempoMode -- the same PAD_INFO.BIN fields the real
// hardware uses for a manually-set tempo -- so the DSP panel can show a previously-detected value
// (see listPads' tempoMode/userBpm) without re-running detection every time it's opened. Response
// is {ok, bpm, saved} rather than just {ok}.
void handleDetectBpm(PluginProcessor& processor, const juce::Array<juce::var>& args,
                      juce::WebBrowserComponent::NativeFunctionCompletion completion) {
    std::optional<double> bpm;
    bool saved = false;

    if (const auto cardRoot = processor.resolveCardRoot(); cardRoot && args.size() > 1 && args[0].toString().length() == 1) {
        const char bankChar = static_cast<char>(args[0].toString()[0]);
        const int indexInBank = static_cast<int>(args[1]);
        bpm = detectBpm(*cardRoot, bankChar, indexInBank);

        if (bpm && indexInBank >= 1 && indexInBank <= Bank::padCount) {
            try {
                const SdCard card = SdCard::load(*cardRoot);
                for (const auto& bank : card.banks()) {
                    if (bank.name != bankChar)
                        continue;
                    PadInfo info = bank.pads[static_cast<size_t>(indexInBank - 1)].info;
                    info.tempoMode = PadInfo::TempoMode::User;
                    info.userTempo = static_cast<uint32_t>(std::lround(*bpm * 10.0));
                    savePadInfo(*cardRoot, bankChar, indexInBank, info);
                    saved = true;
                    break;
                }
            } catch (const std::exception&) {
                saved = false;
            }
        }
    }

    auto* response = new juce::DynamicObject();
    response->setProperty("ok", bpm.has_value());
    if (bpm)
        response->setProperty("bpm", *bpm);
    response->setProperty("saved", saved);
    completion(juce::var(response));
}

// args: [bankChar, indexInBank, timeRatio, pitchSemitones]
void handleApplyPitchTimeStretch(PluginProcessor& processor, const juce::Array<juce::var>& args,
                                  juce::WebBrowserComponent::NativeFunctionCompletion completion) {
    bool ok = false;
    char bankChar = 0;
    if (const auto cardRoot = processor.resolveCardRoot(); cardRoot && args.size() > 3 && args[0].toString().length() == 1) {
        bankChar = static_cast<char>(args[0].toString()[0]);
        ok = applyPitchTimeStretch(*cardRoot, bankChar, static_cast<int>(args[1]), static_cast<double>(args[2]),
                                    static_cast<double>(args[3]));
    }
    if (ok)
        processor.requestBankChange(bankChar);
    respondOk(ok, completion);
}

} // namespace

juce::WebBrowserComponent::Options makeWebViewOptions(PluginProcessor& processor) {
    return juce::WebBrowserComponent::Options{}
        .withNativeIntegrationEnabled()
        .withResourceProvider(provideResource)
        .withNativeFunction("listBanks",
                             [&processor](const juce::Array<juce::var>& args,
                                          juce::WebBrowserComponent::NativeFunctionCompletion completion) {
                                 listBanks(processor, args, completion);
                             })
        .withNativeFunction("listPads",
                             [&processor](const juce::Array<juce::var>& args,
                                          juce::WebBrowserComponent::NativeFunctionCompletion completion) {
                                 listPads(processor, args, completion);
                             })
        .withNativeFunction("selectBank",
                             [&processor](const juce::Array<juce::var>& args,
                                          juce::WebBrowserComponent::NativeFunctionCompletion completion) {
                                 if (args.size() > 0 && args[0].toString().length() == 1)
                                     processor.requestBankChange(static_cast<char>(args[0].toString()[0]));
                                 completion(juce::var());
                             })
        .withNativeFunction("previewPadOn",
                             [&processor](const juce::Array<juce::var>& args,
                                          juce::WebBrowserComponent::NativeFunctionCompletion completion) {
                                 if (args.size() > 0)
                                     processor.previewPadOn(static_cast<int>(args[0]));
                                 completion(juce::var());
                             })
        .withNativeFunction("previewPadOff",
                             [&processor](const juce::Array<juce::var>& args,
                                          juce::WebBrowserComponent::NativeFunctionCompletion completion) {
                                 if (args.size() > 0)
                                     processor.previewPadOff(static_cast<int>(args[0]));
                                 completion(juce::var());
                             })
        .withNativeFunction("updatePad",
                             [&processor](const juce::Array<juce::var>& args,
                                          juce::WebBrowserComponent::NativeFunctionCompletion completion) {
                                 updatePad(processor, args, completion);
                             })
        .withNativeFunction("swapPadSample",
                             [&processor](const juce::Array<juce::var>& args,
                                          juce::WebBrowserComponent::NativeFunctionCompletion completion) {
                                 swapPadSample(processor, args, completion);
                             })
        .withNativeFunction("swapPadSampleFromPath",
                             [&processor](const juce::Array<juce::var>& args,
                                          juce::WebBrowserComponent::NativeFunctionCompletion completion) {
                                 swapPadSampleFromPath(processor, args, completion);
                             })
        .withNativeFunction("getActivePads",
                             [&processor](const juce::Array<juce::var>&,
                                          juce::WebBrowserComponent::NativeFunctionCompletion completion) {
                                 const auto mask = processor.getActivePadMask();
                                 juce::Array<juce::var> indices;
                                 for (int i = 0; i < kPadsPerBank; ++i)
                                     if ((mask & (1u << i)) != 0)
                                         indices.add(i);
                                 completion(juce::var(indices));
                             })
        .withNativeFunction("stopAll",
                             [&processor](const juce::Array<juce::var>&,
                                          juce::WebBrowserComponent::NativeFunctionCompletion completion) {
                                 processor.requestStopAll();
                                 completion(juce::var());
                             })
        .withNativeFunction("getClipping",
                             [&processor](const juce::Array<juce::var>&,
                                          juce::WebBrowserComponent::NativeFunctionCompletion completion) {
                                 completion(juce::var(processor.isClipping()));
                             })
        .withNativeFunction("setKnobValue",
                             [&processor](const juce::Array<juce::var>& args,
                                          juce::WebBrowserComponent::NativeFunctionCompletion completion) {
                                 if (args.size() > 1)
                                     processor.setKnobValue(static_cast<int>(args[0]), static_cast<int>(args[1]));
                                 completion(juce::var());
                             })
        .withNativeFunction("startKnobLearn",
                             [&processor](const juce::Array<juce::var>& args,
                                          juce::WebBrowserComponent::NativeFunctionCompletion completion) {
                                 if (args.size() > 0)
                                     processor.startKnobLearn(static_cast<int>(args[0]));
                                 completion(juce::var());
                             })
        .withNativeFunction("getKnobStates",
                             [&processor](const juce::Array<juce::var>&,
                                          juce::WebBrowserComponent::NativeFunctionCompletion completion) {
                                 juce::Array<juce::var> states;
                                 for (int i = 0; i < kNumKnobs; ++i) {
                                     auto* state = new juce::DynamicObject();
                                     state->setProperty("value", processor.getKnobValue(i));
                                     state->setProperty("cc", processor.getKnobCc(i));
                                     state->setProperty("learning", processor.isKnobLearning(i));
                                     states.add(juce::var(state));
                                 }
                                 completion(juce::var(states));
                             })
        .withNativeFunction("pickZipToOpen",
                             [](const juce::Array<juce::var>&,
                                juce::WebBrowserComponent::NativeFunctionCompletion completion) {
                                 handlePickZipToOpen(completion);
                             })
        .withNativeFunction("pickZipToSave", handlePickZipToSave)
        .withNativeFunction("saveAllBanksToZip",
                             [&processor](const juce::Array<juce::var>& args,
                                          juce::WebBrowserComponent::NativeFunctionCompletion completion) {
                                 handleSaveAllBanks(processor, args, completion);
                             })
        .withNativeFunction("loadAllBanksFromZip",
                             [&processor](const juce::Array<juce::var>& args,
                                          juce::WebBrowserComponent::NativeFunctionCompletion completion) {
                                 handleLoadAllBanks(processor, args, completion);
                             })
        .withNativeFunction("saveBankToZip",
                             [&processor](const juce::Array<juce::var>& args,
                                          juce::WebBrowserComponent::NativeFunctionCompletion completion) {
                                 handleSaveBank(processor, args, completion);
                             })
        .withNativeFunction("peekBankZip", handlePeekBankZip)
        .withNativeFunction("loadBankFromZip",
                             [&processor](const juce::Array<juce::var>& args,
                                          juce::WebBrowserComponent::NativeFunctionCompletion completion) {
                                 handleLoadBank(processor, args, completion);
                             })
        .withNativeFunction("clearBank",
                             [&processor](const juce::Array<juce::var>& args,
                                          juce::WebBrowserComponent::NativeFunctionCompletion completion) {
                                 handleClearBank(processor, args, completion);
                             })
        .withNativeFunction("clearAllBanks",
                             [&processor](const juce::Array<juce::var>& args,
                                          juce::WebBrowserComponent::NativeFunctionCompletion completion) {
                                 handleClearAllBanks(processor, args, completion);
                             })
        .withNativeFunction("getSyncMode",
                             [&processor](const juce::Array<juce::var>& args,
                                          juce::WebBrowserComponent::NativeFunctionCompletion completion) {
                                 handleGetSyncMode(processor, args, completion);
                             })
        .withNativeFunction("enterOfflineMode",
                             [&processor](const juce::Array<juce::var>& args,
                                          juce::WebBrowserComponent::NativeFunctionCompletion completion) {
                                 handleEnterOfflineMode(processor, args, completion);
                             })
        .withNativeFunction("exitOfflineMode",
                             [&processor](const juce::Array<juce::var>& args,
                                          juce::WebBrowserComponent::NativeFunctionCompletion completion) {
                                 handleExitOfflineMode(processor, args, completion);
                             })
        .withNativeFunction("syncMirrorToCard",
                             [&processor](const juce::Array<juce::var>& args,
                                          juce::WebBrowserComponent::NativeFunctionCompletion completion) {
                                 handleSyncMirrorToCard(processor, args, completion);
                             })
        .withNativeFunction("getTheme", handleGetTheme)
        .withNativeFunction("setTheme", handleSetTheme)
        .withNativeFunction("getPadDspStatus",
                             [&processor](const juce::Array<juce::var>& args,
                                          juce::WebBrowserComponent::NativeFunctionCompletion completion) {
                                 handleGetPadDspStatus(processor, args, completion);
                             })
        .withNativeFunction("normalizePad",
                             [&processor](const juce::Array<juce::var>& args,
                                          juce::WebBrowserComponent::NativeFunctionCompletion completion) {
                                 handleNormalizePad(processor, args, completion);
                             })
        .withNativeFunction("convertPadChannels",
                             [&processor](const juce::Array<juce::var>& args,
                                          juce::WebBrowserComponent::NativeFunctionCompletion completion) {
                                 handleConvertPadChannels(processor, args, completion);
                             })
        .withNativeFunction("fadePad",
                             [&processor](const juce::Array<juce::var>& args,
                                          juce::WebBrowserComponent::NativeFunctionCompletion completion) {
                                 handleFadePad(processor, args, completion);
                             })
        .withNativeFunction("trimSilencePad",
                             [&processor](const juce::Array<juce::var>& args,
                                          juce::WebBrowserComponent::NativeFunctionCompletion completion) {
                                 handleTrimSilencePad(processor, args, completion);
                             })
        .withNativeFunction("detectBpm",
                             [&processor](const juce::Array<juce::var>& args,
                                          juce::WebBrowserComponent::NativeFunctionCompletion completion) {
                                 handleDetectBpm(processor, args, completion);
                             })
        .withNativeFunction("applyPitchTimeStretch",
                             [&processor](const juce::Array<juce::var>& args,
                                          juce::WebBrowserComponent::NativeFunctionCompletion completion) {
                                 handleApplyPitchTimeStretch(processor, args, completion);
                             });
}

} // namespace sp404
