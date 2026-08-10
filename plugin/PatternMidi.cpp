#include "PatternMidi.h"

#include <algorithm>
#include <cmath>
#include <vector>

#include "sp404/SdCard.h"

namespace sp404 {

namespace {

// Must match PluginProcessor::kBasePadNote exactly -- not #include "PluginProcessor.h" to get it
// directly, since that header requires JucePlugin_Name and friends to be defined (only true when
// compiled as part of the actual plugin target via juce_add_plugin(), not e.g. from a standalone
// offline test harness). See PatternMidi.h's doc comment for why MIDI export/import deliberately
// reuses this same note-numbering convention.
constexpr int kBasePadNote = 36;

// MIDI ticks/quarter-note -> pattern ticks (384/bar = 96/quarter-note in 4/4).
double midiTicksToPatternTicks(double midiTicks, int ppqn) {
    return midiTicks / ppqn * (kTicksPerBar / 4.0);
}

double patternTicksToMidiTicks(double patternTicks, int ppqn) {
    return patternTicks / (kTicksPerBar / 4.0) * ppqn;
}

// Appends placeholder events covering `gap` pattern ticks, splitting into multiple events since
// PatternEvent::ticksSincePrevious is a single byte (max 255) -- same approach real hardware and
// the community's AudioPattern.fromMidi() reference implementation use.
void appendGap(std::vector<PatternEvent>& events, int gap) {
    while (gap > 255) {
        events.push_back(makePlaceholderEvent(255));
        gap -= 255;
    }
    if (gap > 0)
        events.push_back(makePlaceholderEvent(static_cast<uint8_t>(gap)));
}

} // namespace

bool exportPatternToMidi(const Pattern& pattern, const juce::File& midiDestination, int ppqn) {
    const auto ticks = absoluteEventTicks(pattern);

    juce::MidiMessageSequence sequence;
    bool anyNote = false;

    for (size_t i = 0; i < pattern.events.size(); ++i) {
        const auto& event = pattern.events[i];
        const auto bank = event.bank();
        const auto padIndex = event.padIndexInBank();
        if (!bank || !padIndex)
            continue; // placeholder, or an unrecognized bankSwitch

        anyNote = true;
        const int note = kBasePadNote + (*padIndex - 1);
        const int channel = 1 + (*bank - 'A');

        const double startMidiTicks = patternTicksToMidiTicks(static_cast<double>(ticks[i]), ppqn);
        const double lengthMidiTicks =
            std::max(1.0, patternTicksToMidiTicks(static_cast<double>(event.lengthTicks), ppqn));

        sequence.addEvent(juce::MidiMessage::noteOn(channel, note, static_cast<juce::uint8>(event.velocity)),
                           startMidiTicks);
        sequence.addEvent(juce::MidiMessage::noteOff(channel, note), startMidiTicks + lengthMidiTicks);
    }

    if (!anyNote)
        return false;

    // Explicit end-of-track marker at the pattern's true total length (bars * kTicksPerBar), not
    // wherever the last note happens to end -- a pattern's trailing silence (padding out to a
    // whole bar count) carries no note of its own to anchor a timestamp on otherwise, and without
    // this the reimported pattern would be truncated to its last note's bar. MidiFile::writeTo()
    // only auto-generates its own end-of-track event if the sequence doesn't already have one, so
    // this placement is respected as given (see importPatternFromMidi, which reads it back).
    sequence.addEvent(juce::MidiMessage::endOfTrack(), patternTicksToMidiTicks(pattern.totalTicks(), ppqn));

    sequence.updateMatchedPairs();

    juce::MidiFile midiFile;
    midiFile.setTicksPerQuarterNote(ppqn);
    midiFile.addTrack(sequence);

    midiDestination.deleteFile();
    std::unique_ptr<juce::FileOutputStream> out = midiDestination.createOutputStream();
    if (out == nullptr)
        return false;
    return midiFile.writeTo(*out);
}

std::optional<Pattern> importPatternFromMidi(const juce::File& midiFile) {
    if (!midiFile.existsAsFile())
        return std::nullopt;

    juce::FileInputStream stream(midiFile);
    if (!stream.openedOk())
        return std::nullopt;

    juce::MidiFile file;
    if (!file.readFrom(stream))
        return std::nullopt;

    const int ppqn = file.getTimeFormat();
    if (ppqn <= 0) // negative = SMPTE frames/subframes, not supported
        return std::nullopt;

    struct RawNote {
        double startMidiTicks = 0.0;
        int channel = 0;
        int note = 0;
        int velocity = 0;
        double lengthMidiTicks = 0.0;
    };
    std::vector<RawNote> notes;
    double endOfTrackMidiTicks = 0.0;

    for (int t = 0; t < file.getNumTracks(); ++t) {
        const auto* track = file.getTrack(t);
        if (track == nullptr)
            continue;
        for (int i = 0; i < track->getNumEvents(); ++i) {
            const auto* holder = track->getEventPointer(i);
            const auto& msg = holder->message;
            if (msg.isEndOfTrackMetaEvent()) {
                endOfTrackMidiTicks = std::max(endOfTrackMidiTicks, msg.getTimeStamp());
                continue;
            }
            if (!msg.isNoteOn())
                continue;
            // NOTE: JUCE pairs each note-on with the *next* note-off of the same note+channel
            // (see MidiMessageSequence::updateMatchedPairs, which readFrom() already calls).
            // When the same pad retriggers before its previous hit's note-off -- a real pattern
            // can absolutely do this -- lengthMidiTicks below may end up shorter than the
            // original length that was exported. Onset tick/velocity/pad are unaffected, and this
            // plugin's own playback already cuts a still-sounding pad on retrigger (one voice per
            // pad, see README), so an imprecise length on a retriggered hit doesn't change how
            // the pattern actually plays -- accepted as a known Standard-MIDI-File limitation
            // rather than something worth a more elaborate encoding to work around.
            double lengthMidiTicks = 0.0;
            if (holder->noteOffObject != nullptr)
                lengthMidiTicks = holder->noteOffObject->message.getTimeStamp() - msg.getTimeStamp();
            notes.push_back({msg.getTimeStamp(), msg.getChannel(), msg.getNoteNumber(), msg.getVelocity(),
                              lengthMidiTicks});
        }
    }

    std::sort(notes.begin(), notes.end(),
              [](const RawNote& a, const RawNote& b) { return a.startMidiTicks < b.startMidiTicks; });

    Pattern pattern;
    int lastPatternTick = 0;
    // Seed with the end-of-track marker's own position (see exportPatternToMidi) so a pattern's
    // trailing silence -- padding out to a whole bar count with no note to anchor a timestamp on
    // -- survives the round-trip instead of being truncated to the last note's bar.
    int maxPatternTick = static_cast<int>(std::llround(midiTicksToPatternTicks(endOfTrackMidiTicks, ppqn)));

    for (const auto& raw : notes) {
        const int bankIndex = raw.channel - 1; // MIDI channel 1 = bank A
        if (bankIndex < 0 || bankIndex >= SdCard::numBanks)
            continue;
        const char bank = static_cast<char>('A' + bankIndex);
        const int padIndex = raw.note - kBasePadNote + 1;
        if (padIndex < 1 || padIndex > Bank::padCount)
            continue;

        const int patternTick =
            static_cast<int>(std::llround(midiTicksToPatternTicks(raw.startMidiTicks, ppqn)));
        const int lengthPatternTicks =
            static_cast<int>(std::llround(midiTicksToPatternTicks(raw.lengthMidiTicks, ppqn)));

        appendGap(pattern.events, patternTick - lastPatternTick);

        const int clampedVelocity = juce::jlimit(0, 127, raw.velocity);
        const int clampedLength = juce::jlimit(0, 65535, lengthPatternTicks);
        pattern.events.push_back(makeNoteEvent(bank, padIndex, 0, static_cast<uint8_t>(clampedVelocity),
                                                static_cast<uint16_t>(clampedLength)));

        lastPatternTick = patternTick;
        maxPatternTick = std::max(maxPatternTick, patternTick);
    }

    if (pattern.events.empty())
        return std::nullopt;

    // Round up to a whole number of bars (minimum 1), padding with a trailing placeholder --
    // mirrors how a pattern recorded on real hardware always covers whole bars.
    const int bars = std::max(1, (maxPatternTick + kTicksPerBar - 1) / kTicksPerBar);
    appendGap(pattern.events, bars * kTicksPerBar - lastPatternTick);

    pattern.bars = bars;
    pattern.timeSignature = 0; // always import as 4/4 -- no attempt to detect/preserve the source file's own signature

    return pattern;
}

} // namespace sp404
