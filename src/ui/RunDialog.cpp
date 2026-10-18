#include "ui/SeparationDialog.h"

#include "song/Separation.h"
#include "ui/Colours.h"

#include <cmath>

namespace overdub::ui
{
namespace
{
constexpr int panelWidth = 460;
constexpr int panelHeight = 186;
} // namespace

std::string separationTitleText (const std::string& sourceName)
{
    return "Separating " + sourceName;
}

std::string separationHintText()
{
    std::string parts;
    for (std::size_t i = 0; i < song::separationParts.size(); ++i)
    {
        if (i > 0)
            parts += i + 1 == song::separationParts.size() ? " and " : ", ";
        parts += song::separationParts[i].fileStem;
    }
    return "Splitting it into " + parts + ". This takes several times the song's length.";
}

std::string separationStatusText (const song::SeparationProgress& progress)
{
    if (progress.chunks <= 0)
        return "Starting\xe2\x80\xa6";
    const auto percent = (int) std::floor (progress.fraction() * 100.0);
    return "Chunk " + std::to_string (progress.chunksDone) + " of " + std::to_string (progress.chunks) + " \xc2\xb7 " + std::to_string (percent) + "%";
}

SeparationDialog::SeparationDialog()
{
    title.setFont (juce::FontOptions (16.0f, juce::Font::bold));
    title.setColour (juce::Label::textColourId, colours::text);
    hint.setText (juce::String::fromUTF8 (separationHintText().c_str()), juce::dontSendNotification);
    hint.setColour (juce::Label::textColourId, colours::dimText);
    hint.setFont (juce::FontOptions (12.0f));
    statusLabel.setColour (juce::Label::textColourId, colours::dimText);
    addAndMakeVisible (title);
    addAndMakeVisible (hint);
    addAndMakeVisible (progressBar);
    addAndMakeVisible (statusLabel);
    addAndMakeVisible (cancelButton);
    cancelButton.onClick = [this] {
        if (onCancel)
            onCancel();
    };
    // Covers the window: every click lands here, none on the page under it.
    setInterceptsMouseClicks (true, true);
}

void SeparationDialog::show (const std::string& sourceName, const song::SeparationProgress& progress)
{
    title.setText (juce::String::fromUTF8 (separationTitleText (sourceName).c_str()), juce::dontSendNotification);
    statusLabel.setText (juce::String::fromUTF8 (separationStatusText (progress).c_str()), juce::dontSendNotification);
    progressBar.setFraction (progress.fraction());
}

juce::Rectangle<int> SeparationDialog::panelBounds() const
{
    return getLocalBounds().withSizeKeepingCentre (std::min (panelWidth, getWidth() - 32), panelHeight);
}

void SeparationDialog::resized()
{
    auto area = panelBounds().reduced (20, 16);
    title.setBounds (area.removeFromTop (24));
    area.removeFromTop (4);
    hint.setBounds (area.removeFromTop (34));
    area.removeFromTop (10);
    progressBar.setBounds (area.removeFromTop (8));
    area.removeFromTop (8);
    statusLabel.setBounds (area.removeFromTop (20));
    area.removeFromTop (8);
    cancelButton.setBounds (area.removeFromTop (26).removeFromRight (90));
}

void SeparationDialog::paint (juce::Graphics& g)
{
    g.fillAll (juce::Colours::black.withAlpha (0.55f));
    const auto panel = panelBounds().toFloat();
    g.setColour (colours::panel);
    g.fillRoundedRectangle (panel, 10.0f);
    g.setColour (colours::line);
    g.drawRoundedRectangle (panel.reduced (0.5f), 10.0f, 1.0f);
}
} // namespace overdub::ui
