#pragma once

#include <juce_audio_basics/juce_audio_basics.h>

#include <optional>

#include "sp404/Pattern.h"

namespace sp404 {

// Converts a pattern's events to a single-track Standard MIDI File and writes it to
// midiDestination -- lets a pattern be inspected/edited in any DAW, independent of this plugin.
// Each pad is mapped to MIDI note (PluginProcessor::kBasePadNote + padIndexInBank - 1) on channel
// (1 + bank - 'A') -- the *same* note numbers this plugin's own live pad-triggering already uses
// (see PluginProcessor::handleMidiMessage), with the bank distinguished by channel instead of by
// note, so every one of the 120 possible pads maps to a valid, non-overlapping note/channel pair
// and a MIDI controller mapped to trigger this plugin can also trigger the exported file's notes
// directly. ppqn is the MIDI file's pulses-per-quarter-note (unrelated to the pattern's own
// native 96 PPQN, i.e. sp404::kTicksPerBar -- export rescales to whatever a target DAW expects,
// 480 by default). Placeholder events carry no note and are skipped. Returns false if the pattern
// has no real (non-placeholder) events, or the file can't be written.
bool exportPatternToMidi(const Pattern& pattern, const juce::File& midiDestination, int ppqn = 480);

// Reverse of exportPatternToMidi(): reads a Standard MIDI File and quantizes every note-on onto
// the pattern's native 384-ticks/bar grid (sp404::kTicksPerBar), mapping MIDI channel/note back
// to a bank/pad using the same convention (channel 1-10 = bank A-J, note kBasePadNote..
// kBasePadNote+11 = pad 1-12 within that bank) -- independent of what note numbers/channels the
// file's own tracks otherwise use; anything outside that channel 1-10 + note range is silently
// skipped, along with all non-note-on content (tempo, CC, meta events). Multiple tracks are
// merged into one time-sorted sequence. The resulting pattern's length is rounded up to a whole
// number of bars (minimum 1), padded with placeholder events -- mirroring how real hardware pads
// out a recorded pattern. Returns std::nullopt if the file can't be read, or contains no note
// that maps to a valid pad.
std::optional<Pattern> importPatternFromMidi(const juce::File& midiFile);

} // namespace sp404
