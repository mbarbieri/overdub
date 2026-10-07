#include "ui/Pages.h"

#include "ui/Colours.h"

namespace overdub::ui
{
PlaceholderPage::PlaceholderPage (const juce::String& text)
{
    message.setText (text, juce::dontSendNotification);
    message.setJustificationType (juce::Justification::centred);
    message.setColour (juce::Label::textColourId, colours::dimText);
    message.setFont (juce::FontOptions (16.0f));
    addAndMakeVisible (message);
}

void PlaceholderPage::resized()
{
    message.setBounds (getLocalBounds());
}

void PlaceholderPage::paint (juce::Graphics& g)
{
    g.fillAll (colours::background);
}

NoSongPage::NoSongPage()
{
    message.setText ("No Song open", juce::dontSendNotification);
    message.setJustificationType (juce::Justification::centred);
    message.setColour (juce::Label::textColourId, colours::dimText);
    message.setFont (juce::FontOptions (18.0f));
    newSongButton.onClick = [this] {
        if (onNewSong)
            onNewSong();
    };
    openSongButton.onClick = [this] {
        if (onOpenSong)
            onOpenSong();
    };
    for (auto* button : { &newSongButton, &openSongButton })
    {
        button->setWantsKeyboardFocus (false);
        addAndMakeVisible (button);
    }
    addAndMakeVisible (message);
}

void NoSongPage::resized()
{
    auto area = getLocalBounds().withSizeKeepingCentre (260, 90);
    message.setBounds (area.removeFromTop (40));
    area.removeFromTop (10);
    newSongButton.setBounds (area.removeFromLeft (125));
    openSongButton.setBounds (area.removeFromRight (125));
}

void NoSongPage::paint (juce::Graphics& g)
{
    g.fillAll (colours::background);
}

NoticeLine::NoticeLine()
{
    setInterceptsMouseClicks (false, false);
}

void NoticeLine::show (const juce::String& newSentence)
{
    sentence = newSentence;
    setVisible (true);
    repaint();
    startTimer (visibleForMs);
}

void NoticeLine::dismiss()
{
    stopTimer();
    sentence.clear();
    setVisible (false);
}

void NoticeLine::paint (juce::Graphics& g)
{
    g.setColour (colours::notice.withAlpha (0.95f));
    g.fillRect (getLocalBounds());
    g.setColour (colours::text);
    g.setFont (juce::FontOptions (13.0f));
    g.drawText (sentence, getLocalBounds().reduced (12, 0), juce::Justification::centredLeft);
}
} // namespace overdub::ui
