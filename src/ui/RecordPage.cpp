#include "ui/RecordPage.h"

#include "ui/Colours.h"
#include "ui/Layout.h"
#include "ui/MuteSoloButtons.h"
#include "ui/TakeLanes.h"

#include <algorithm>

namespace overdub::ui
{
int preRollChoiceIndex (song::PreRoll preRoll)
{
    return (int) std::distance (preRollChoices.begin(), std::ranges::find (preRollChoices, preRoll));
}

RecordPageState recordPageStateFor (const song::Song& song,
                                    const std::vector<std::string>& inputChannelNames,
                                    const std::function<bool (const song::Id&)>& isMonitoring,
                                    bool passRunning)
{
    RecordPageState state;
    state.canImport = ! passRunning;
    state.preRoll = song.preRoll;
    state.canChangePreRoll = ! passRunning;
    const auto choices = inputChoices (inputChannelNames);
    for (const auto& track : song.tracks)
    {
        state.rows.push_back (TrackRowState {
            .trackId = track.id,
            .name = track.name,
            .armed = track.armed,
            .mute = track.mute,
            .solo = track.solo,
            .choices = choices,
            .selectedChoice = choiceIndexFor (choices, track.inputChannels),
            .monitoring = isMonitoring (track.id),
            .enabled = ! passRunning,
        });
    }
    return state;
}

//==============================================================================

TrackRow::TrackRow()
{
    armButton.setColour (juce::TextButton::buttonOnColourId, colours::rec);
    monitoringButton.setColour (juce::TextButton::buttonOnColourId, colours::accent);
    setUpMuteSoloButtons (muteButton, soloButton, [this] { return current.mute; }, [this] { return current.solo; }, onMute, onSolo);
    nameLabel.setColour (juce::Label::textColourId, colours::text);
    nameLabel.setFont (juce::FontOptions (14.0f));
    inputPicker.setTextWhenNothingSelected ("Input?");

    armButton.onClick = [this] {
        if (onArm)
            onArm (! current.armed);
    };
    monitoringButton.onClick = [this] {
        if (onMonitoring)
            onMonitoring (! current.monitoring);
    };
    inputPicker.onChange = [this] {
        const auto index = inputPicker.getSelectedItemIndex();
        if (index < 0 || (std::size_t) index >= current.choices.size() || ! onInput)
            return;
        if (current.selectedChoice == (std::size_t) index)
            return;
        onInput (current.choices[(std::size_t) index].channels);
    };

    for (auto* button : { &armButton, &monitoringButton })
        button->setWantsKeyboardFocus (false);
    inputPicker.setWantsKeyboardFocus (false);

    addAndMakeVisible (armButton);
    addAndMakeVisible (nameLabel);
    addAndMakeVisible (muteButton);
    addAndMakeVisible (soloButton);
    addAndMakeVisible (inputPicker);
    addAndMakeVisible (monitoringButton);
}

void TrackRow::show (const TrackRowState& state)
{
    const auto choicesChanged = state.choices != current.choices;
    current = state;

    nameLabel.setText (juce::String::fromUTF8 (state.name.c_str()), juce::dontSendNotification);
    armButton.setToggleState (state.armed, juce::dontSendNotification);
    monitoringButton.setToggleState (state.monitoring, juce::dontSendNotification);
    muteButton.setToggleState (state.mute, juce::dontSendNotification);
    soloButton.setToggleState (state.solo, juce::dontSendNotification);

    if (choicesChanged)
    {
        inputPicker.clear (juce::dontSendNotification);
        for (std::size_t i = 0; i < state.choices.size(); ++i)
            inputPicker.addItem (juce::String::fromUTF8 (state.choices[i].label.c_str()), (int) i + 1);
    }
    if (state.selectedChoice.has_value())
        inputPicker.setSelectedItemIndex ((int) *state.selectedChoice, juce::dontSendNotification);
    else
        inputPicker.setSelectedId (0, juce::dontSendNotification);

    for (auto* control : std::initializer_list<juce::Component*> { &armButton, &muteButton, &soloButton, &inputPicker, &monitoringButton })
        control->setEnabled (state.enabled);
    repaint();
}

void TrackRow::resized()
{
    const auto layout = trackRowLayout (getLocalBounds());
    armButton.setBounds (layout.arm);
    nameLabel.setBounds (layout.name);
    muteButton.setBounds (layout.mute);
    soloButton.setBounds (layout.solo);
    inputPicker.setBounds (layout.input);
    monitoringButton.setBounds (layout.monitoring);
}

void TrackRow::paint (juce::Graphics& g)
{
    const auto area = getLocalBounds().toFloat().reduced (0.5f);
    g.setColour (colours::panel);
    g.fillRoundedRectangle (area, 7.0f);
    g.setColour (current.armed ? colours::rec : colours::line);
    g.drawRoundedRectangle (area, 7.0f, 1.0f);
}

//==============================================================================

RecordPage::RecordPage()
{
    importButton.setWantsKeyboardFocus (false);
    importButton.onClick = [this] {
        if (onImport)
            onImport();
    };
    emptyLabel.setText ("No Tracks yet. Import an audio file to play along to.", juce::dontSendNotification);
    emptyLabel.setColour (juce::Label::textColourId, colours::dimText);

    preRollCaption.setColour (juce::Label::textColourId, colours::dimText);
    preRollCaption.setJustificationType (juce::Justification::centredRight);
    for (std::size_t i = 0; i < preRollChoices.size(); ++i)
    {
        const auto seconds = song::preRollSeconds (preRollChoices[i]);
        preRollPicker.addItem (seconds == 0 ? juce::String ("Off") : juce::String (seconds) + " s", (int) i + 1);
    }
    preRollPicker.setSelectedItemIndex (preRollChoiceIndex (shownPreRoll), juce::dontSendNotification);
    preRollPicker.setWantsKeyboardFocus (false);
    preRollPicker.onChange = [this] {
        const auto index = preRollPicker.getSelectedItemIndex();
        if (index < 0 || (std::size_t) index >= preRollChoices.size() || preRollChoices[(std::size_t) index] == shownPreRoll)
            return;
        if (onPreRoll)
            onPreRoll (preRollChoices[(std::size_t) index]);
    };

    passesList.onSelect = [this] (const song::Id& passId) {
        if (onJumpToPass)
            onJumpToPass (passId);
    };
    passesList.onBringBack = [this] (const song::Id& passId) {
        if (onBringBack)
            onBringBack (passId);
    };
    overview.onSeek = [this] (song::SampleCount sample) {
        if (onSeek)
            onSeek (sample);
    };

    addAndMakeVisible (importButton);
    addChildComponent (emptyLabel);
    addAndMakeVisible (preRollCaption);
    addAndMakeVisible (preRollPicker);
    addAndMakeVisible (passesList);
    addAndMakeVisible (lookAheadStrip);
    addAndMakeVisible (overview);
}

void RecordPage::show (const RecordPageState& state)
{
    const auto sameTracks = rows.size() == state.rows.size()
                         && std::equal (rows.begin(), rows.end(), state.rows.begin(), [] (const auto& row, const auto& rowState) { return row->trackId() == rowState.trackId; });
    if (! sameTracks)
    {
        rows.clear();
        for (const auto& rowState : state.rows)
        {
            auto row = std::make_unique<TrackRow>();
            const auto trackId = rowState.trackId;
            row->onArm = [this, trackId] (bool armed) {
                if (onArm)
                    onArm (trackId, armed);
            };
            row->onInput = [this, trackId] (std::vector<int> channels) {
                if (onInput)
                    onInput (trackId, std::move (channels));
            };
            row->onMonitoring = [this, trackId] (bool on) {
                if (onMonitoring)
                    onMonitoring (trackId, on);
            };
            row->onMute = [this, trackId] (bool mute) {
                if (onMute)
                    onMute (trackId, mute);
            };
            row->onSolo = [this, trackId] (bool solo) {
                if (onSolo)
                    onSolo (trackId, solo);
            };
            addAndMakeVisible (*row);
            rows.push_back (std::move (row));
        }
        resized();
    }

    for (std::size_t i = 0; i < rows.size(); ++i)
        rows[i]->show (state.rows[i]);
    importButton.setEnabled (state.canImport);
    emptyLabel.setVisible (rows.empty());

    shownPreRoll = state.preRoll;
    preRollPicker.setSelectedItemIndex (preRollChoiceIndex (state.preRoll), juce::dontSendNotification);
    preRollPicker.setEnabled (state.canChangePreRoll);
}

void RecordPage::showSong (const song::Song& song, const RecordTransport& newTransport, bool editable)
{
    const auto changed = ! shownSong.has_value() || shownSong->sampleRate != song.sampleRate || shownSong->tracks != song.tracks
                      || shownSong->passes != song.passes || shownSong->takes != song.takes || shownEditable != editable;
    if (changed)
    {
        shownSong = song;
        shownEditable = editable;
        const auto rowsBefore = passesList.numRows();
        passesList.show (passRows (song), editable);
        if (passesList.numRows() != rowsBefore)
            resized();
        envelope = peaksFor ? backingEnvelope (song, peaksFor) : BackingEnvelope {};
    }
    transport = newTransport;
    showLookAhead();
    overview.show (song, transport.playhead);
}

void RecordPage::showLookAhead()
{
    if (! shownSong.has_value())
        return;
    lookAheadStrip.show (lookAheadView (envelope, transport, shownSong->sampleRate, lookAheadStrip.waveformBounds().getWidth()), shownSong->sampleRate);
}

void RecordPage::resized()
{
    const auto layout = recordPageLayout (getLocalBounds(), (int) rows.size());
    stageArea = layout.stage;
    for (std::size_t i = 0; i < rows.size(); ++i)
        rows[i]->setBounds (layout.rows[i]);
    importButton.setBounds (layout.importButton);
    emptyLabel.setBounds (layout.importButton.withWidth (trackColumnWidth).translated (0, layout.importButton.getHeight() + 6));
    preRollCaption.setBounds (layout.preRollCaption);
    preRollPicker.setBounds (layout.preRollPicker);
    passesList.setBounds (layout.passes);
    lookAheadStrip.setBounds (layout.lookAhead);
    overview.setBounds (layout.overview);
    showLookAhead(); // its columns follow the strip's width
}

void RecordPage::paint (juce::Graphics& g)
{
    g.fillAll (colours::background);
    g.setColour (colours::panel.withAlpha (0.45f));
    g.fillRoundedRectangle (stageArea.toFloat(), 8.0f);
    g.setColour (colours::line);
    g.drawRoundedRectangle (stageArea.toFloat().reduced (0.5f), 8.0f, 1.0f);
    g.setColour (colours::dimText);
    g.setFont (juce::FontOptions (13.0f));
    g.drawText ("Camera stage: the camera arrives in a later version.", stageArea, juce::Justification::centred);
}
} // namespace overdub::ui
