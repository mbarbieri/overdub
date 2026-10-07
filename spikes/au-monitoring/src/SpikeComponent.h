// PROTOTYPE — issue #6 spike. Manual controls plus a live state readout and log.
#pragma once
#include "SpikeEngine.h"
#include "PluginWindow.h"
#include <juce_audio_utils/juce_audio_utils.h>

class SpikeComponent final : public juce::Component, private juce::Timer
{
public:
    explicit SpikeComponent (SpikeEngine&);
    ~SpikeComponent() override;
    void resized() override;
    void appendLog (const juce::String& line);

private:
    struct TrackRow
    {
        juce::Label label;
        juce::ComboBox input, plugin;
        juce::ToggleButton outOfProcess { "out-of-process" }, monitor { "monitor" }, armed { "armed" };
        juce::TextButton load { "Load" }, edit { "Edit" }, unload { "Remove" };
    };

    void timerCallback() override;
    void setUpTrackRow (int track);
    void openEditor (int track);
    void closeEditor (int track);

    SpikeEngine& engine;
    juce::AudioDeviceSelectorComponent deviceSelector;
    TrackRow rows[SpikeEngine::numTracks];
    juce::TextButton loadBacking { "Load backing..." }, record { "Play + record armed" },
        stop { "Stop" }, loopback { "Loopback test" };
    juce::Label status;
    juce::TextEditor log;
    std::unique_ptr<PluginWindow> editors[SpikeEngine::numTracks];
    std::unique_ptr<juce::FileChooser> chooser;
};
