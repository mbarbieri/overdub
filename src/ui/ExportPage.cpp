#include "ui/ExportPage.h"

#include "ui/Colours.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace overdub::ui
{
namespace
{
constexpr int formWidth = 560;
constexpr int radioGroup = 3701;

// "1:23.4": minutes and seconds to a tenth, rounded down.
std::string minutesText (song::SampleCount samples, int sampleRate)
{
    const auto tenths = sampleRate > 0 && samples > 0 ? samples * 10 / sampleRate : 0;
    char text[32];
    std::snprintf (text, sizeof (text), "%lld:%02lld.%lld", (long long) (tenths / 600), (long long) (tenths / 10 % 60), (long long) (tenths % 10));
    return text;
}

void styleHeading (juce::Label& label, const juce::String& text)
{
    label.setText (text, juce::dontSendNotification);
    label.setColour (juce::Label::textColourId, colours::dimText);
    label.setFont (juce::FontOptions (11.0f));
}
} // namespace

void ExportProgressBar::setFraction (double fraction)
{
    const auto clamped = std::clamp (fraction, 0.0, 1.0);
    if (std::abs (clamped - value) < 1.0e-12)
        return;
    value = clamped;
    repaint();
}

void ExportProgressBar::paint (juce::Graphics& g)
{
    const auto area = getLocalBounds().toFloat();
    const auto radius = area.getHeight() / 2.0f;
    g.setColour (juce::Colours::black);
    g.fillRoundedRectangle (area, radius);
    if (value <= 0.0)
        return;
    g.setColour (colours::accent);
    g.fillRoundedRectangle (area.withWidth (std::max (area.getHeight(), (float) (area.getWidth() * value))), radius);
}

std::string exportSpanText (song::SampleCount clipsEnd, int sampleRate)
{
    return "Whole mix: 0:00.0 \xe2\x80\x93 " + minutesText (clipsEnd, sampleRate) + ", plus effect tails";
}

std::string exportStatusText (const ExportPageState& state)
{
    if (state.waitingForAudio)
        return "Preparing\xe2\x80\xa6";
    if (state.rendering)
        return "Exporting\xe2\x80\xa6 " + std::to_string ((int) std::floor (state.progress * 100.0)) + "%";
    if (state.finishedFile.has_value())
        return "Exported to " + state.finishedFile->getFullPathName().toStdString();
    return {};
}

ExportPage::ExportPage()
{
    title.setText ("Export", juce::dontSendNotification);
    title.setColour (juce::Label::textColourId, colours::text);
    title.setFont (juce::FontOptions (16.0f, juce::Font::bold));
    styleHeading (formatHeading, "Format");
    styleHeading (destinationHeading, "Save as");
    spanLabel.setColour (juce::Label::textColourId, colours::dimText);
    destinationLabel.setColour (juce::Label::textColourId, colours::text);
    destinationLabel.setColour (juce::Label::backgroundColourId, juce::Colours::black.withAlpha (0.4f));
    destinationLabel.setMinimumHorizontalScale (0.7f);
    statusLabel.setColour (juce::Label::textColourId, colours::dimText);

    const std::pair<juce::ToggleButton*, exporting::ExportFormat> formatButtons[] {
        { &wavButton, exporting::ExportFormat::wav },
        { &m4aButton, exporting::ExportFormat::m4a },
        { &mp4Button, exporting::ExportFormat::mp4 },
    };
    for (const auto& [button, buttonFormat] : formatButtons)
    {
        button->setRadioGroupId (radioGroup, juce::dontSendNotification);
        button->setWantsKeyboardFocus (false);
        button->onClick = [this, format = buttonFormat] { chooseFormat (format); };
        addAndMakeVisible (button);
    }
    wavButton.setToggleState (true, juce::dontSendNotification);

    chooseButton.onClick = [this] {
        if (onChooseDestination)
            onChooseDestination (chosenDestination);
    };
    exportButton.onClick = [this] {
        if (onExport)
            onExport (request());
    };
    exportButton.setColour (juce::TextButton::buttonColourId, colours::accent);
    revealButton.onClick = [this] {
        if (onReveal && shown.finishedFile.has_value())
            onReveal (*shown.finishedFile);
    };

    for (auto* button : { &chooseButton, &exportButton, &revealButton })
        button->setWantsKeyboardFocus (false);
    for (juce::Component* child : { (juce::Component*) &title, (juce::Component*) &formatHeading, (juce::Component*) &spanLabel, (juce::Component*) &destinationHeading, (juce::Component*) &destinationLabel, (juce::Component*) &chooseButton, (juce::Component*) &exportButton, (juce::Component*) &progressBar, (juce::Component*) &statusLabel })
        addAndMakeVisible (child);
    addChildComponent (revealButton);
    updateControls();
}

void ExportPage::show (const ExportPageState& state)
{
    shown = state;
    progressBar.setFraction (state.rendering ? state.progress : (state.finishedFile.has_value() ? 1.0 : 0.0));
    spanLabel.setText (juce::String::fromUTF8 (exportSpanText (state.clipsEnd, state.sampleRate).c_str()), juce::dontSendNotification);
    statusLabel.setText (juce::String::fromUTF8 (exportStatusText (state).c_str()), juce::dontSendNotification);
    updateControls();
}

void ExportPage::setDestination (const juce::File& file)
{
    chosenDestination = file == juce::File() ? file : exporting::withFormatExtension (file, chosenFormat);
    destinationLabel.setText (chosenDestination.getFullPathName(), juce::dontSendNotification);
    updateControls();
}

void ExportPage::chooseFormat (exporting::ExportFormat format)
{
    chosenFormat = format;
    setDestination (chosenDestination);
}

void ExportPage::updateControls()
{
    const auto busy = shown.waitingForAudio || shown.rendering;
    for (auto* button : { &wavButton, &m4aButton })
        button->setEnabled (! busy);
    mp4Button.setEnabled (false); // slice 17
    chooseButton.setEnabled (! busy);
    exportButton.setEnabled (! busy && exporting::isAvailable (chosenFormat) && chosenDestination != juce::File());
    revealButton.setVisible (! busy && shown.finishedFile.has_value());
}

void ExportPage::resized()
{
    auto area = getLocalBounds().reduced (30);
    area = area.withSizeKeepingCentre (std::min (formWidth, area.getWidth()), area.getHeight());

    title.setBounds (area.removeFromTop (28));
    area.removeFromTop (8);
    formatHeading.setBounds (area.removeFromTop (18));
    for (auto* button : { &wavButton, &m4aButton, &mp4Button })
        button->setBounds (area.removeFromTop (24));
    area.removeFromTop (8);
    spanLabel.setBounds (area.removeFromTop (22));
    area.removeFromTop (10);
    destinationHeading.setBounds (area.removeFromTop (18));
    auto destinationRow = area.removeFromTop (26);
    chooseButton.setBounds (destinationRow.removeFromRight (90));
    destinationRow.removeFromRight (6);
    destinationLabel.setBounds (destinationRow);
    area.removeFromTop (14);
    auto actions = area.removeFromTop (28);
    exportButton.setBounds (actions.removeFromRight (100));
    actions.removeFromRight (6);
    revealButton.setBounds (actions.removeFromRight (140));
    area.removeFromTop (12);
    progressBar.setBounds (area.removeFromTop (10));
    area.removeFromTop (6);
    statusLabel.setBounds (area.removeFromTop (22));
}

void ExportPage::paint (juce::Graphics& g)
{
    g.fillAll (colours::background);
}
} // namespace overdub::ui
