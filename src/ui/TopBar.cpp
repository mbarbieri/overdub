#include "ui/TopBar.h"

#include "ui/Colours.h"
#include "ui/Layout.h"

namespace overdub::ui
{
namespace
{
const juce::String rewindGlyph = juce::String (juce::CharPointer_UTF8 ("\xe2\x8f\xae"));
const juce::String playGlyph = juce::String (juce::CharPointer_UTF8 ("\xe2\x96\xb6"));
const juce::String stopGlyph = juce::String (juce::CharPointer_UTF8 ("\xe2\x96\xa0"));
const juce::String recordGlyph = juce::String (juce::CharPointer_UTF8 ("\xe2\x97\x8f"));
} // namespace

void ClockDisplay::show (const std::string& newTime, const std::string& newStatus, bool recording)
{
    const auto timeString = juce::String::fromUTF8 (newTime.c_str());
    const auto statusString = juce::String::fromUTF8 (newStatus.c_str());
    if (timeString == time && statusString == status && recording == isRecording)
        return;
    time = timeString;
    status = statusString;
    isRecording = recording;
    repaint();
}

void ClockDisplay::paint (juce::Graphics& g)
{
    auto area = getLocalBounds();
    g.setColour (juce::Colours::black);
    g.fillRoundedRectangle (area.toFloat(), 5.0f);

    auto statusArea = area.removeFromBottom (13);
    g.setColour (colours::text);
    g.setFont (juce::FontOptions (17.0f, juce::Font::bold));
    g.drawText (time, area, juce::Justification::centred);
    g.setColour (isRecording ? colours::rec : colours::dimText);
    g.setFont (juce::FontOptions (9.5f));
    g.drawText (status, statusArea.translated (0, -2), juce::Justification::centred);
}

TopBar::TopBar()
{
    rewindButton.setButtonText (rewindGlyph);
    playStopButton.setButtonText (playGlyph);
    recordButton.setButtonText (recordGlyph);
    recordButton.setColour (juce::TextButton::textColourOffId, colours::rec);
    recordButton.setColour (juce::TextButton::buttonOnColourId, colours::rec);
    recordButton.setColour (juce::TextButton::textColourOnId, juce::Colours::white);

    for (auto* modeButton : { &recordModeButton, &editModeButton, &exportModeButton })
    {
        modeButton->setClickingTogglesState (false);
        modeButton->setColour (juce::TextButton::buttonOnColourId, colours::accent);
    }

    recordModeButton.onClick = [this] { send (Command::showRecord); };
    editModeButton.onClick = [this] { send (Command::showEdit); };
    exportModeButton.onClick = [this] { send (Command::showExport); };
    rewindButton.onClick = [this] { send (Command::rewind); };
    playStopButton.onClick = [this] { send (Command::playOrStop); };
    recordButton.onClick = [this] { send (Command::record); };
    undoButton.onClick = [this] { send (Command::undo); };
    redoButton.onClick = [this] { send (Command::redo); };

    for (auto* button : { &recordModeButton, &editModeButton, &exportModeButton, &rewindButton, &playStopButton, &recordButton, &undoButton, &redoButton })
    {
        button->setWantsKeyboardFocus (false); // keys go to the main window (MainComponent::keyPressed)
        addAndMakeVisible (button);
    }
    addAndMakeVisible (clock);
}

void TopBar::send (Command command)
{
    if (onCommand)
        onCommand (command);
}

void TopBar::show (const TopBarState& state)
{
    recordModeButton.setToggleState (state.mode == Mode::record, juce::dontSendNotification);
    editModeButton.setToggleState (state.mode == Mode::edit, juce::dontSendNotification);
    exportModeButton.setToggleState (state.mode == Mode::exportSong, juce::dontSendNotification);

    for (auto* transport : { &rewindButton, &playStopButton, &recordButton })
        transport->setEnabled (state.songOpen);
    playStopButton.setButtonText (state.playing || state.recording ? stopGlyph : playGlyph);
    recordButton.setToggleState (state.recording, juce::dontSendNotification);
    clock.show (state.clock, state.status, state.recording);

    undoButton.setButtonText (juce::String::fromUTF8 (state.undoText.c_str()));
    redoButton.setButtonText (juce::String::fromUTF8 (state.redoText.c_str()));
    undoButton.setEnabled (state.canUndo);
    redoButton.setEnabled (state.canRedo);
}

void TopBar::resized()
{
    const auto layout = topBarLayout (getLocalBounds());
    recordModeButton.setBounds (layout.modeButtons[0]);
    editModeButton.setBounds (layout.modeButtons[1]);
    exportModeButton.setBounds (layout.modeButtons[2]);
    rewindButton.setBounds (layout.rewind);
    playStopButton.setBounds (layout.playStop);
    recordButton.setBounds (layout.record);
    clock.setBounds (layout.clock);
    undoButton.setBounds (layout.undo);
    redoButton.setBounds (layout.redo);
}

void TopBar::paint (juce::Graphics& g)
{
    g.fillAll (colours::panel);
    g.setColour (colours::line);
    g.fillRect (getLocalBounds().removeFromBottom (1));
}
} // namespace overdub::ui
