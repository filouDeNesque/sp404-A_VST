#pragma once

#include <juce_gui_extra/juce_gui_extra.h>

namespace sp404 {

class PluginProcessor;

// Builds the WebBrowserComponent::Options used by PluginEditor: serves the embedded
// web/index.html through a ResourceProvider and exposes "listBanks"/"listPads"/"selectBank"/
// "previewPadOn"/"previewPadOff"/"updatePad"/"getActivePads"/"getClipping"/"setKnobValue"/
// "startKnobLearn"/"getKnobStates" NativeFunctions.
// listBanks/listPads are backed by sp404::findConnectedCardRoot() + sp404::SdCard
// (auto-detects a mounted SP-404SX SD card under /Volumes; macOS only for now). listBanks also
// reports totalBytes/freeBytes (std::filesystem::space() on the card's mount point) when a card
// is connected, for the UI's SD icon tooltip. selectBank arms
// `processor` for MIDI playback of that bank (see PluginProcessor::requestBankChange) --
// clicking a bank in the UI both displays its pads and makes it the active bank for incoming
// MIDI notes, same as pressing a bank button on the hardware. previewPadOn/Off (pad index 0-11
// within the active bank) let the UI audition a pad on mousedown/mouseup, same trigger path as a
// real MIDI note (see PluginProcessor::previewPadOn/Off). updatePad(bankChar, indexInBank 1-12,
// updates) edits volume/loop/gate/reverse/lofi and writes immediately to PAD_INFO.BIN on the
// real SD card (see sp404::savePadInfo) -- no separate save step, no undo. getActivePads()
// returns the pad indices (0-11) of the active bank that are currently sounding, for the UI to
// poll and highlight (see PluginProcessor::getActivePadMask). stopAll() immediately fades out
// every currently-sounding pad (see PluginProcessor::requestStopAll) -- the UI's Cancel/Stop
// button. getClipping() returns whether the output has clipped in roughly the last 500ms (see
// PluginProcessor::isClipping), for the UI to flash the screen graphic red. setKnobValue(index
// 0-3, value 0-127) sets a knob from a UI drag; startKnobLearn(index) arms a knob to capture the
// next incoming MIDI CC; getKnobStates() returns [{value, cc, learning}, ...] for the 4 knobs,
// polled to drive their rotation and MIDI-learn indicator (see PluginProcessor's knob methods).
juce::WebBrowserComponent::Options makeWebViewOptions(PluginProcessor& processor);

} // namespace sp404
