// PROTOTYPE — issue #6 spike. A plugin editor in its own DocumentWindow (ADR 0001).
#pragma once
#include "SpikeLog.h"
#include <juce_audio_processors/juce_audio_processors.h>

class PluginWindow final : public juce::DocumentWindow
{
public:
    PluginWindow (juce::AudioPluginInstance& plugin, std::function<void()> onCloseRequested)
        : juce::DocumentWindow (plugin.getName(), juce::Colours::darkgrey, juce::DocumentWindow::closeButton),
          onClose (std::move (onCloseRequested))
    {
        // Set native title bar BEFORE adding the editor: changing it later recreates the NSWindow and
        // re-parents the plugin NSView, which crashed NeuralAmpModeler in-process (spike finding).
        setUsingNativeTitleBar (true);
        juce::AudioProcessorEditor* editor = plugin.createEditorAndMakeActive();
        kind = "native";

        if (editor == nullptr)
        {
            editor = new juce::GenericAudioProcessorEditor (plugin);
            kind = "generic_fallback";
        }

        setContentOwned (editor, true);
        setResizable (editor->isResizable(), false);
        centreWithSize (juce::jmax (200, getWidth()), juce::jmax (100, getHeight()));
        setVisible (true);
        spikelog::line ("editor_opened", { { "plugin", plugin.getName() }, { "kind", kind },
                                           { "w", juce::String (editor->getWidth()) }, { "h", juce::String (editor->getHeight()) } });
    }

    juce::StringPairArray describe() const
    {
        auto* content = getContentComponent();
        juce::StringPairArray fields;
        fields.set ("kind", kind);
        fields.set ("w", juce::String (content != nullptr ? content->getWidth() : 0));
        fields.set ("h", juce::String (content != nullptr ? content->getHeight() : 0));
        return fields;
    }

    void closeButtonPressed() override { onClose(); }

private:
    std::function<void()> onClose;
    juce::String kind;
};
