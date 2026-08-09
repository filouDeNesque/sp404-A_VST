#include "BankLoader.h"

#include <optional>

#include "sp404/SdCard.h"

namespace sp404 {

std::shared_ptr<const LoadedBank> loadBank(const std::filesystem::path& cardRoot, char bankName,
                                            juce::AudioFormatManager& formatManager,
                                            const std::function<bool()>& shouldAbort) {
    std::optional<SdCard> card;
    try {
        card = SdCard::load(cardRoot);
    } catch (const std::exception&) {
        return nullptr;
    }

    const Bank* bank = nullptr;
    for (const auto& b : card->banks()) {
        if (b.name == bankName) {
            bank = &b;
            break;
        }
    }
    if (bank == nullptr)
        return nullptr;

    auto loaded = std::make_shared<LoadedBank>();
    loaded->name = bankName;

    for (size_t i = 0; i < bank->pads.size(); ++i) {
        if (shouldAbort())
            return nullptr;

        const auto& pad = bank->pads[i];
        auto& loadedPad = loaded->pads[i];
        loadedPad.info = pad.info;

        if (!pad.samplePath)
            continue;

        const juce::File wavFile(juce::String(pad.samplePath->string().c_str()));
        std::unique_ptr<juce::AudioFormatReader> reader(formatManager.createReaderFor(wavFile));
        if (reader == nullptr)
            continue;

        loadedPad.buffer.setSize(static_cast<int>(reader->numChannels),
                                  static_cast<int>(reader->lengthInSamples));
        reader->read(&loadedPad.buffer, 0, static_cast<int>(reader->lengthInSamples), 0, true, true);
        loadedPad.hasSample = true;
    }

    return loaded;
}

BankLoader::BankLoader(std::function<std::optional<std::filesystem::path>()> resolveCardRootFn)
    : juce::Thread("SP404 Bank Loader"), resolveCardRoot(std::move(resolveCardRootFn)) {
    formatManager.registerBasicFormats();
    startThread();
}

BankLoader::~BankLoader() {
    // Generous timeout: shutdown races a per-pad file read (see loadBank's shouldAbort
    // checkpoints), and a single slow read on a sluggish SD card reader can occasionally take
    // longer than a tighter timeout would allow, forcing an unsafe thread kill.
    stopThread(5000);
}

void BankLoader::requestBank(char bankName) {
    requestedBankName.store(bankName, std::memory_order_relaxed);
}

std::shared_ptr<const LoadedBank> BankLoader::getCurrentBank() const {
    const juce::SpinLock::ScopedLockType lock(bankLock);
    return currentBank;
}

void BankLoader::run() {
    while (!threadShouldExit()) {
        const char requested = requestedBankName.exchange(0, std::memory_order_relaxed);

        if (requested != 0) {
            if (const auto cardRoot = resolveCardRoot()) {
                auto loaded = loadBank(*cardRoot, requested, formatManager,
                                        [this] { return threadShouldExit(); });
                if (loaded != nullptr) {
                    const juce::SpinLock::ScopedLockType lock(bankLock);
                    currentBank = std::move(loaded);
                }
            }
        }

        wait(30);
    }
}

} // namespace sp404
