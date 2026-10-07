#include "ui/DeviceSettingsPanel.h"

#include "ui/Colours.h"

#include <algorithm>

namespace overdub::ui
{
namespace
{
constexpr int rowHeight = 30;
constexpr int labelWidth = 110;

void fill (juce::ComboBox& picker, const std::vector<juce::String>& items, const juce::String& selected)
{
    picker.clear (juce::dontSendNotification);
    for (std::size_t i = 0; i < items.size(); ++i)
        picker.addItem (items[i], (int) i + 1);
    const auto found = std::ranges::find (items, selected);
    picker.setSelectedItemIndex (found == items.end() ? 0 : (int) std::distance (items.begin(), found), juce::dontSendNotification);
}

std::vector<juce::String> names (const std::vector<std::string>& values)
{
    std::vector<juce::String> result;
    for (const auto& value : values)
        result.push_back (juce::String::fromUTF8 (value.c_str()));
    return result;
}

std::vector<juce::String> numbers (const std::vector<int>& values, const char* unit)
{
    std::vector<juce::String> result;
    for (const auto value : values)
        result.push_back (juce::String (value) + unit);
    return result;
}

template <typename T>
T pickedFrom (const std::vector<T>& values, const juce::ComboBox& picker, T fallback)
{
    const auto index = picker.getSelectedItemIndex();
    return index >= 0 && (std::size_t) index < values.size() ? values[(std::size_t) index] : fallback;
}
} // namespace

DeviceSettingsPanel::DeviceSettingsPanel()
{
    applyButton.onClick = [this] {
        if (onApply)
            onApply (picked());
    };
    for (auto* label : { &inputLabel, &outputLabel, &sampleRateLabel, &bufferSizeLabel })
    {
        label->setColour (juce::Label::textColourId, colours::text);
        addAndMakeVisible (label);
    }
    for (auto* picker : { &inputPicker, &outputPicker, &sampleRatePicker, &bufferSizePicker })
        addAndMakeVisible (picker);
    addAndMakeVisible (applyButton);
}

void DeviceSettingsPanel::show (const DeviceChoices& choices)
{
    offered = choices;
    fill (inputPicker, names (choices.inputDevices), juce::String::fromUTF8 (choices.current.inputDevice.c_str()));
    fill (outputPicker, names (choices.outputDevices), juce::String::fromUTF8 (choices.current.outputDevice.c_str()));
    fill (sampleRatePicker, numbers (choices.sampleRates, " Hz"), juce::String (choices.current.sampleRate) + " Hz");
    fill (bufferSizePicker, numbers (choices.bufferSizes, " samples"), juce::String (choices.current.bufferSize) + " samples");
}

engine::DeviceSettings DeviceSettingsPanel::picked() const
{
    return engine::DeviceSettings {
        .deviceType = offered.deviceType,
        .inputDevice = pickedFrom (offered.inputDevices, inputPicker, std::string {}),
        .outputDevice = pickedFrom (offered.outputDevices, outputPicker, std::string {}),
        .sampleRate = pickedFrom (offered.sampleRates, sampleRatePicker, 0),
        .bufferSize = pickedFrom (offered.bufferSizes, bufferSizePicker, 0),
    };
}

void DeviceSettingsPanel::resized()
{
    auto area = getLocalBounds().reduced (16);
    const std::array<std::pair<juce::Label*, juce::ComboBox*>, 4> rows {
        { { &inputLabel, &inputPicker }, { &outputLabel, &outputPicker }, { &sampleRateLabel, &sampleRatePicker }, { &bufferSizeLabel, &bufferSizePicker } }
    };
    for (auto& [label, picker] : rows)
    {
        auto row = area.removeFromTop (rowHeight);
        label->setBounds (row.removeFromLeft (labelWidth));
        picker->setBounds (row);
        area.removeFromTop (8);
    }
    applyButton.setBounds (area.removeFromTop (rowHeight).removeFromRight (100));
}

void DeviceSettingsPanel::paint (juce::Graphics& g)
{
    g.fillAll (colours::background);
}
} // namespace overdub::ui
