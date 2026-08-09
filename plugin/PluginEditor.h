#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_gui_extra/juce_gui_extra.h>

#include "PluginProcessor.h"

namespace sp404 {

class PluginEditor final : public juce::AudioProcessorEditor {
public:
    explicit PluginEditor(PluginProcessor&);

    void resized() override;

private:
    juce::WebBrowserComponent webView;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(PluginEditor)
};

} // namespace sp404
