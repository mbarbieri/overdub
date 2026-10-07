#pragma once

#include "engine/DeviceSettings.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>
#include <string>
#include <vector>

// The settings window's audio device picker (issue #27): input device, output device, sample
// rate and buffer size. The choice is per machine (engine/DeviceSettings.h), never in the Song.
// Apply hands the picked settings to the app, which saves them and reopens the device.
namespace overdub::ui
{
// What the picker offers, and the machine's current choice.
struct DeviceChoices
{
    std::string deviceType;
    std::vector<std::string> inputDevices;
    std::vector<std::string> outputDevices;
    std::vector<int> sampleRates;
    std::vector<int> bufferSizes;
    engine::DeviceSettings current;
};

class DeviceSettingsPanel final : public juce::Component
{
public:
    DeviceSettingsPanel();

    void show (const DeviceChoices& choices);
    void resized() override;
    void paint (juce::Graphics& g) override;

    std::function<void (const engine::DeviceSettings&)> onApply;

    juce::ComboBox inputPicker;
    juce::ComboBox outputPicker;
    juce::ComboBox sampleRatePicker;
    juce::ComboBox bufferSizePicker;
    juce::TextButton applyButton { "Apply" };

private:
    engine::DeviceSettings picked() const;

    DeviceChoices offered;
    juce::Label inputLabel { {}, "Input" };
    juce::Label outputLabel { {}, "Output" };
    juce::Label sampleRateLabel { {}, "Sample rate" };
    juce::Label bufferSizeLabel { {}, "Buffer size" };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (DeviceSettingsPanel)
};
} // namespace overdub::ui
