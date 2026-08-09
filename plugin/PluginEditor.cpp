#include "PluginEditor.h"

#include "WebUIBridge.h"

namespace sp404 {

PluginEditor::PluginEditor(PluginProcessor& proc)
    : AudioProcessorEditor(proc), webView(makeWebViewOptions(proc)) {
    setResizable(true, true);
    setResizeLimits(500, 360, 1400, 1000);
    setSize(700, 850);
    addAndMakeVisible(webView);
    webView.goToURL(juce::WebBrowserComponent::getResourceProviderRoot());
}

void PluginEditor::resized() {
    webView.setBounds(getLocalBounds());
}

} // namespace sp404
