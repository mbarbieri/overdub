#pragma once

#include "engine/DeviceSettings.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>
#include <string>
#include <vector>

// The settings window's audio device picker (issue #27): input device, output device, sample
// rate and buffer size. The choice is per machine (engine/DeviceSettings.h), never in the Song.
// Apply hands the picked settings to the app, which saves them and reopens the device.
//
// The rate and buffer size pickers list what the picked devices offer (issue #159): choosing
// another input or output asks offerForDevices and lists its answer, keeping the picked rate and
// buffer size when offered and the nearest offered value otherwise.
namespace overdub::ui
{
// The sample rates and buffer sizes a pair of devices offers: with different input and output
// devices, only the values both offer.
struct DeviceOffer
{
    std::vector<int> sampleRates;
    std::vector<int> bufferSizes;

    bool operator== (const DeviceOffer&) const = default;
};

// The values in both, in `first`'s order.
std::vector<int> offeredByBoth (const std::vector<int>& first, const std::vector<int>& second);

// `wanted` when offered, else the offered value nearest to it (the higher one on a tie); 0 when
// nothing is offered.
int keptOrNearest (const std::vector<int>& offered, int wanted);

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
    // What a pair of devices (names as listed) offers. Unset: the lists stay as show() gave them.
    std::function<DeviceOffer (const std::string& inputDevice, const std::string& outputDevice)> offerForDevices;

    juce::ComboBox inputPicker;
    juce::ComboBox outputPicker;
    juce::ComboBox sampleRatePicker;
    juce::ComboBox bufferSizePicker;
    juce::TextButton applyButton { "Apply" };

private:
    engine::DeviceSettings picked() const;
    // Lists what the picked devices offer (offerForDevices).
    void listPickedDevicesOffer();

    DeviceChoices offered;
    juce::Label inputLabel { {}, "Input" };
    juce::Label outputLabel { {}, "Output" };
    juce::Label sampleRateLabel { {}, "Sample rate" };
    juce::Label bufferSizeLabel { {}, "Buffer size" };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (DeviceSettingsPanel)
};
} // namespace overdub::ui
