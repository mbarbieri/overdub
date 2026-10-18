#include "ui/DeviceSettingsPanel.h"

#include "core/Log.h"
#include "ui/Colours.h"

#include <algorithm>
#include <cstdlib>

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

std::vector<int> offeredByBoth (const std::vector<int>& first, const std::vector<int>& second)
{
    std::vector<int> both;
    for (const auto value : first)
        if (std::ranges::contains (second, value))
            both.push_back (value);
    return both;
}

int keptOrNearest (const std::vector<int>& offered, int wanted)
{
    int nearest = 0;
    for (const auto value : offered)
    {
        const auto distance = std::abs (value - wanted);
        const auto nearestDistance = std::abs (nearest - wanted);
        if (nearest == 0 || distance < nearestDistance || (distance == nearestDistance && value > nearest))
            nearest = value;
    }
    return nearest;
}

DeviceSettingsPanel::DeviceSettingsPanel()
{
    inputPicker.onChange = [this] { listPickedDevicesOffer(); };
    outputPicker.onChange = [this] { listPickedDevicesOffer(); };
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

void DeviceSettingsPanel::listPickedDevicesOffer()
{
    if (! offerForDevices)
        return;
    const auto current = picked();
    const auto offer = offerForDevices (current.inputDevice, current.outputDevice);
    core::logEvent (core::Level::debug,
                    "ui.device_offer_listed",
                    { { "input_device", current.inputDevice }, { "output_device", current.outputDevice }, { "sample_rates", offer.sampleRates }, { "buffer_sizes", offer.bufferSizes } });
    offered.sampleRates = offer.sampleRates;
    offered.bufferSizes = offer.bufferSizes;
    fill (sampleRatePicker, numbers (offer.sampleRates, " Hz"), juce::String (keptOrNearest (offer.sampleRates, current.sampleRate)) + " Hz");
    fill (bufferSizePicker, numbers (offer.bufferSizes, " samples"), juce::String (keptOrNearest (offer.bufferSizes, current.bufferSize)) + " samples");
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
