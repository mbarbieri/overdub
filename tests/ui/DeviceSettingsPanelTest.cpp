#include "ui/DeviceSettingsPanel.h"

#include "ui/UiHarness.h"

#include <catch2/catch_test_macros.hpp>

#include <optional>
#include <string>
#include <utility>
#include <vector>

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

TEST_CASE ("a value the picked devices still offer stays picked; otherwise the nearest offered one is", "[ui][settings]")
{
    using overdub::ui::keptOrNearest;
    using overdub::ui::offeredByBoth;
    CHECK (keptOrNearest ({ 44100, 48000 }, 44100) == 44100);
    CHECK (keptOrNearest ({ 48000, 96000 }, 44100) == 48000);
    CHECK (keptOrNearest ({ 44100, 48000 }, 46050) == 48000); // a tie goes to the higher
    CHECK (keptOrNearest ({ 64, 128 }, 512) == 128);
    CHECK (keptOrNearest ({}, 48000) == 0);
    CHECK (offeredByBoth ({ 44100, 48000, 96000 }, { 96000, 48000 }) == std::vector<int> { 48000, 96000 });
    CHECK (offeredByBoth ({ 44100 }, { 48000 }).empty());
}

TEST_CASE ("choosing other devices lists what they offer, and Apply hands back a rate they offer", "[ui][settings]")
{
    juce::ScopedJuceInitialiser_GUI gui;
    DeviceSettingsPanel panel;
    layOut (panel, 420, 220);
    std::vector<std::pair<std::string, std::string>> asked;
    panel.offerForDevices = [&] (const std::string& input, const std::string& output) {
        asked.emplace_back (input, output);
        if (input == "Interface" && output == "Interface")
            return overdub::ui::DeviceOffer { { 48000, 96000 }, { 64, 128 } };
        return overdub::ui::DeviceOffer { { 44100, 48000 }, { 128, 256, 512 } };
    };
    auto start = choices();
    start.current.sampleRate = 44100;
    panel.show (start);
    std::optional<DeviceSettings> applied;
    panel.onApply = [&] (const DeviceSettings& settings) { applied = settings; };

    choose (panel.inputPicker, 1);
    choose (panel.outputPicker, 1);
    CHECK (asked.back() == std::pair<std::string, std::string> { "Interface", "Interface" });
    REQUIRE (panel.sampleRatePicker.getNumItems() == 2);
    CHECK (panel.sampleRatePicker.getItemText (0) == "48000 Hz");
    CHECK (panel.sampleRatePicker.getItemText (1) == "96000 Hz");
    CHECK (panel.sampleRatePicker.getText() == "48000 Hz"); // 44100 Hz is not offered
    CHECK (panel.bufferSizePicker.getText() == "128 samples"); // nearest to 256

    click (panel.applyButton);
    REQUIRE (applied.has_value());
    CHECK (*applied == DeviceSettings { "CoreAudio", "Interface", "Interface", 48000, 128 });
}
