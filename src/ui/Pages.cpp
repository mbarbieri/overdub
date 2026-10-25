#include "ui/Pages.h"

#include "ui/Colours.h"

namespace overdub::ui
{
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
    // Only the action button takes clicks; the rest of the line lets them through.
    setInterceptsMouseClicks (false, true);
    actionButton.setWantsKeyboardFocus (false);
    actionButton.onClick = [this] {
        auto clicked = std::move (action);
        dismiss();
        if (clicked)
            clicked();
    };
    addChildComponent (actionButton);
}

void NoticeLine::show (const juce::String& newSentence, const juce::String& actionLabel, std::function<void()> onAction)
{
    sentence = newSentence;
    action = std::move (onAction);
    actionButton.setButtonText (actionLabel);
    actionButton.setVisible (action != nullptr);
    setVisible (true);
    resized();
    repaint();
    hidesAt.reset();
    if (action != nullptr)
        stopTimer();
    else
    {
        hidesAt = juce::Time::currentTimeMillis() + visibleForMs;
        startTimer (visibleForMs);
    }
}

void NoticeLine::dismiss()
{
    stopTimer();
    hidesAt.reset();
    sentence.clear();
    action = nullptr;
    actionButton.setVisible (false);
    setVisible (false);
}

void NoticeLine::resized()
{
    actionButton.setBounds (getLocalBounds().reduced (6, 3).removeFromRight (150));
}

void NoticeLine::paint (juce::Graphics& g)
{
    g.setColour (colours::notice.withAlpha (0.95f));
    g.fillRect (getLocalBounds());
    g.setColour (colours::text);
    g.setFont (juce::FontOptions (13.0f));
    auto textArea = getLocalBounds().reduced (12, 0);
    if (actionButton.isVisible())
        textArea.removeFromRight (actionButton.getWidth() + 12);
    g.drawText (sentence, textArea, juce::Justification::centredLeft);
}
} // namespace overdub::ui
