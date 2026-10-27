#include "ui/TopBar.h"

#include "ui/Colours.h"
#include "ui/Layout.h"

#include "song/Transpose.h"

#include <cctype>
#include <charconv>
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

std::optional<int> parseTranspose (const std::string& text)
{
    auto trimmed = juce::String::fromUTF8 (text.c_str()).trim();
    bool negative = false;
    if (trimmed.startsWithChar ('+'))
        trimmed = trimmed.substring (1);
    else if (trimmed.startsWithChar ('-') || trimmed.startsWithChar ((juce::juce_wchar) 0x2212))
    {
        negative = true;
        trimmed = trimmed.substring (1);
    }
    const auto digits = trimmed.toStdString();
    if (digits.empty() || ! std::isdigit ((unsigned char) digits.front()))
        return std::nullopt;
    int value = 0;
    const auto [end, error] = std::from_chars (digits.data(), digits.data() + digits.size(), value);
    if (error != std::errc() || end != digits.data() + digits.size())
        return std::nullopt;
    return negative ? -value : value;
}

void Spinner::visibilityChanged()
{
    if (isVisible())
        startTimerHz (20);
    else
        stopTimer();
}

void Spinner::timerCallback()
{
    step = (step + 1) % 20;
    repaint();
}

void Spinner::paint (juce::Graphics& g)
{
    const auto area = getLocalBounds().toFloat().reduced (2.0f);
    const auto start = juce::MathConstants<float>::twoPi * (float) step / 20.0f;
    juce::Path arc;
    arc.addCentredArc (area.getCentreX(), area.getCentreY(), area.getWidth() / 2.0f, area.getHeight() / 2.0f, 0.0f, start, start + 4.0f, true);
    g.setColour (colours::accent);
    g.strokePath (arc, juce::PathStrokeType (2.0f));
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

    transposeCaption.setColour (juce::Label::textColourId, colours::dimText);
    transposeCaption.setFont (juce::FontOptions (9.5f, juce::Font::bold));
    transposeCaption.setBorderSize ({});
    transposeUnit.setColour (juce::Label::textColourId, colours::dimText);
    transposeUnit.setFont (juce::FontOptions (11.0f));
    transposeUnit.setBorderSize ({});
    for (auto* label : { &transposeCaption, &transposeUnit })
    {
        label->setInterceptsMouseClicks (false, false);
        addAndMakeVisible (label);
    }
    transposeField.setEditable (true, false, false);
    transposeField.setFont (juce::FontOptions (12.0f));
    transposeField.setJustificationType (juce::Justification::centred);
    transposeField.setColour (juce::Label::textColourId, colours::text);
    transposeField.setColour (juce::Label::outlineColourId, colours::line);
    transposeField.setText (shownTranspose, juce::dontSendNotification);
    transposeField.onTextChange = [this] { transposeEntered(); };
    addAndMakeVisible (transposeField);
    transposeSpinner.setInterceptsMouseClicks (false, false);
    addChildComponent (transposeSpinner);
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

void TopBar::transposeEntered()
{
    const auto typed = parseTranspose (transposeField.getText().toStdString());
    // The field shows the Song's value until the handler has changed it (show() is called back).
    transposeField.setText (juce::String::fromUTF8 (shownTranspose.c_str()), juce::dontSendNotification);
    if (typed.has_value())
        send (SongIntent { intent::SetTranspose { *typed } });
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

    shownTranspose = song::transposeText (state.transpose);
    if (! transposeField.isBeingEdited())
        transposeField.setText (juce::String::fromUTF8 (shownTranspose.c_str()), juce::dontSendNotification);
    transposeField.setColour (juce::Label::backgroundColourId, state.transpose != 0 ? colours::accent : colours::background);
    transposeField.setEnabled (state.songOpen && state.canTranspose);
    transposeSpinner.setVisible (state.songOpen && state.transposeRendering);
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
    transposeCaption.setBounds (layout.transposeCaption);
    transposeField.setBounds (layout.transposeField);
    transposeUnit.setBounds (layout.transposeUnit);
    transposeSpinner.setBounds (layout.transposeSpinner);
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
