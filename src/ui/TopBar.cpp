#include "ui/TopBar.h"

#include "ui/Colours.h"
#include "ui/Layout.h"

#include <cmath>

namespace overdub::ui
{
namespace
{
const juce::String rewindGlyph = juce::String (juce::CharPointer_UTF8 ("\xe2\x8f\xae"));
const juce::String playGlyph = juce::String (juce::CharPointer_UTF8 ("\xe2\x96\xb6"));
const juce::String stopGlyph = juce::String (juce::CharPointer_UTF8 ("\xe2\x96\xa0"));
const juce::String recordGlyph = juce::String (juce::CharPointer_UTF8 ("\xe2\x97\x8f"));
const juce::String cancelGlyph = juce::String (juce::CharPointer_UTF8 ("\xe2\x9c\x95"));
constexpr int cancelExportButtonWidth = 24;
} // namespace

std::string exportStatusText (bool waitingForAudio, double progress)
{
    if (waitingForAudio)
        return "Preparing\xe2\x80\xa6";
    return "Exporting\xe2\x80\xa6 " + std::to_string ((int) std::floor (progress * 100.0)) + "%";
}

std::string controlIndicatorName (const std::vector<std::string>& processes)
{
    if (processes.empty())
        return {};
    auto text = processes.front();
    if (processes.size() > 1)
        text += " +" + std::to_string (processes.size() - 1);
    return text;
}

void ControlIndicator::show (const std::string& newName)
{
    const auto nameString = juce::String::fromUTF8 (newName.c_str());
    if (nameString == name)
        return;
    name = nameString;
    repaint();
}

void ControlIndicator::paint (juce::Graphics& g)
{
    if (name.isEmpty())
        return;
    auto area = getLocalBounds();
    auto captionArea = area.removeFromTop (14);
    g.setColour (colours::mute);
    g.setFont (juce::FontOptions (9.5f, juce::Font::bold));
    g.drawText (caption, captionArea, juce::Justification::bottomLeft, true);
    g.setColour (colours::text);
    g.setFont (juce::FontOptions (13.0f, juce::Font::bold));
    g.drawText (name, area, juce::Justification::centredLeft, true);
}

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

    for (auto* modeButton : { &recordModeButton, &editModeButton })
    {
        modeButton->setClickingTogglesState (false);
        modeButton->setColour (juce::TextButton::buttonOnColourId, colours::accent);
    }

    recordModeButton.onClick = [this] { send (WindowIntent { intent::ShowMode { Mode::record } }); };
    editModeButton.onClick = [this] { send (WindowIntent { intent::ShowMode { Mode::edit } }); };
    rewindButton.onClick = [this] { send (SongIntent { intent::Rewind {} }); };
    playStopButton.onClick = [this] { send (SongIntent { intent::PlayOrStop {} }); };
    recordButton.onClick = [this] { send (SongIntent { intent::Record {} }); };
    undoButton.onClick = [this] { send (SongIntent { intent::Undo {} }); };
    redoButton.onClick = [this] { send (SongIntent { intent::Redo {} }); };
    exportButton.onClick = [this] { send (SongIntent { intent::ShowExport { true } }); };
    cancelExportButton.setButtonText (cancelGlyph);
    cancelExportButton.setTooltip ("Cancel Export");
    cancelExportButton.onClick = [this] { send (SongIntent { intent::CancelExport {} }); };
    cancelExportButton.setWantsKeyboardFocus (false);

    for (auto* button : { &recordModeButton, &editModeButton, &rewindButton, &playStopButton, &recordButton, &undoButton, &redoButton, &exportButton })
    {
        button->setWantsKeyboardFocus (false); // keys go to the main window (MainComponent::keyPressed)
        addAndMakeVisible (button);
    }
    addAndMakeVisible (clock);
    exportStatusLabel.setColour (juce::Label::textColourId, colours::dimText);
    exportStatusLabel.setFont (juce::FontOptions (11.0f));
    exportStatusLabel.setInterceptsMouseClicks (false, false);
    addChildComponent (exportStatusLabel);
    addChildComponent (exportProgressBar);
    addChildComponent (cancelExportButton);

    controlIndicator.setInterceptsMouseClicks (false, false);
    addAndMakeVisible (controlIndicator);
}

void TopBar::send (const Intent& intent)
{
    if (onIntent)
        onIntent (intent);
}

void TopBar::show (const TopBarState& state)
{
    recordModeButton.setToggleState (state.mode == Mode::record, juce::dontSendNotification);
    editModeButton.setToggleState (state.mode == Mode::edit, juce::dontSendNotification);

    for (auto* transport : { &rewindButton, &playStopButton, &recordButton })
        transport->setEnabled (state.songOpen);
    playStopButton.setButtonText (state.playing || state.recording ? stopGlyph : playGlyph);
    recordButton.setToggleState (state.recording, juce::dontSendNotification);
    clock.show (state.clock, state.status, state.recording);

    undoButton.setButtonText (juce::String::fromUTF8 (state.undoText.c_str()));
    redoButton.setButtonText (juce::String::fromUTF8 (state.redoText.c_str()));
    undoButton.setEnabled (state.canUndo);
    redoButton.setEnabled (state.canRedo);

    const auto exporting = state.exportStatus.has_value();
    exportButton.setVisible (! exporting);
    exportButton.setEnabled (state.canExport);
    exportStatusLabel.setVisible (exporting);
    exportProgressBar.setVisible (exporting);
    cancelExportButton.setVisible (exporting);
    cancelExportButton.setEnabled (state.canCancelExport);
    exportStatusLabel.setText (juce::String::fromUTF8 (state.exportStatus.value_or ("").c_str()), juce::dontSendNotification);
    exportProgressBar.setFraction (state.exportProgress);
    controlIndicator.show (controlIndicatorName (state.controllingProcesses));
}

void TopBar::resized()
{
    const auto layout = topBarLayout (getLocalBounds());
    recordModeButton.setBounds (layout.modeButtons[0]);
    editModeButton.setBounds (layout.modeButtons[1]);
    rewindButton.setBounds (layout.rewind);
    playStopButton.setBounds (layout.playStop);
    recordButton.setBounds (layout.record);
    clock.setBounds (layout.clock);
    undoButton.setBounds (layout.undo);
    redoButton.setBounds (layout.redo);
    exportButton.setBounds (layout.exportSlot);
    auto exportSlot = layout.exportSlot;
    cancelExportButton.setBounds (exportSlot.removeFromRight (cancelExportButtonWidth));
    exportSlot.removeFromRight (4);
    exportStatusLabel.setBounds (exportSlot.removeFromTop (exportSlot.getHeight() - 10));
    exportProgressBar.setBounds (exportSlot.removeFromTop (6));
    controlIndicator.setBounds (layout.controlIndicator);
}

void TopBar::paint (juce::Graphics& g)
{
    g.fillAll (colours::panel);
    g.setColour (colours::line);
    g.fillRect (getLocalBounds().removeFromBottom (1));
}
} // namespace overdub::ui
