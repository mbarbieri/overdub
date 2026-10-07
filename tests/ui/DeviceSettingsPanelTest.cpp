#include "ui/DeviceSettingsPanel.h"

#include "ui/UiHarness.h"

#include <catch2/catch_test_macros.hpp>

#include <optional>

using overdub::engine::DeviceSettings;
using overdub::ui::DeviceChoices;
using overdub::ui::DeviceSettingsPanel;
using namespace overdub::ui::testing;

namespace
{
DeviceChoices choices()
{
    return DeviceChoices {
        .deviceType = "CoreAudio",
        .inputDevices = { "Built-in Microphone", "Interface" },
        .outputDevices = { "Built-in Output", "Interface" },
        .sampleRates = { 44100, 48000 },
        .bufferSizes = { 128, 256, 512 },
        .current = DeviceSettings { "CoreAudio", "Built-in Microphone", "Built-in Output", 48000, 256 },
    };
}
} // namespace

TEST_CASE ("the device picker shows the machine's current choice", "[ui][settings]")
{
    juce::ScopedJuceInitialiser_GUI gui;
    DeviceSettingsPanel panel;
    layOut (panel, 420, 220);
    panel.show (choices());

    CHECK (panel.inputPicker.getText() == "Built-in Microphone");
    CHECK (panel.outputPicker.getText() == "Built-in Output");
    CHECK (panel.sampleRatePicker.getText() == "48000 Hz");
    CHECK (panel.bufferSizePicker.getText() == "256 samples");
}

TEST_CASE ("Apply hands back the picked devices, rate and buffer size", "[ui][settings]")
{
    juce::ScopedJuceInitialiser_GUI gui;
    DeviceSettingsPanel panel;
    layOut (panel, 420, 220);
    panel.show (choices());
    std::optional<DeviceSettings> applied;
    panel.onApply = [&] (const DeviceSettings& settings) { applied = settings; };

    choose (panel.inputPicker, 1);
    choose (panel.outputPicker, 1);
    choose (panel.sampleRatePicker, 0);
    choose (panel.bufferSizePicker, 2);
    click (panel.applyButton);

    REQUIRE (applied.has_value());
    CHECK (*applied == DeviceSettings { "CoreAudio", "Interface", "Interface", 44100, 512 });
}
