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

std::string monitoringText (int activeInserts)
{
    if (activeInserts <= 0)
        return "Monitoring";
    return "Monitoring \xC2\xB7 " + std::to_string (activeInserts) + " fx";
}

CameraControlsState cameraControlsStateFor (const song::Song& song, const std::vector<capture::CameraDevice>& cameras, bool passRunning)
{
    CameraControlsState state;
    state.armed = song.cameraArmed;
    state.devices = cameras;
    const auto chosen = std::ranges::find (cameras, song.cameraDeviceId, &capture::CameraDevice::id);
    if (chosen != cameras.end())
        state.selected = (std::size_t) std::distance (cameras.begin(), chosen);
    state.canArm = ! passRunning && (song.cameraArmed || ! cameras.empty());
    state.canChoose = ! passRunning;
    return state;
}

bool stageShowsLiveFeed (bool cameraArmed, bool passRunning, bool playing)
{
    return cameraArmed && (passRunning || ! playing);
}

std::string cameraStageMessage (bool armed)
{
    return armed ? "Camera Armed: waiting for its picture." : "Arm the camera to record a Video Take with each Pass.";
}

RecordPageState recordPageStateFor (const song::Song& song,
                                    const std::vector<std::string>& inputChannelNames,
                                    const std::function<bool (const song::Id&)>& isMonitoring,
                                    bool passRunning,
                                    const InsertStatusLookup& insertStatus)
{
    RecordPageState state;
    state.canAddTracks = ! passRunning;
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
            .inserts = insertChipsFor (track.insertChain, insertStatus),
            .activeInserts = activeInsertCount (track.insertChain, insertStatus),
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
    nameLabel.setInterceptsMouseClicks (false, false); // a click on the name is the row's
    addAndMakeVisible (nameLabel);
    addAndMakeVisible (muteButton);
    addAndMakeVisible (soloButton);
    addAndMakeVisible (inputPicker);
    addAndMakeVisible (monitoringButton);
    addAndMakeVisible (insertChain);
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
    monitoringButton.setButtonText (juce::String::fromUTF8 (monitoringText (state.activeInserts).c_str()));
    insertChain.show (state.trackId, state.inserts, state.enabled);

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
    insertChain.setBounds (layout.insertChain);
}

void TrackRow::mouseDown (const juce::MouseEvent& event)
{
    if (event.mods.isPopupMenu() && onMenu)
        onMenu();
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
    addTrackButton.setWantsKeyboardFocus (false);
    addTrackButton.onClick = [this] {
        if (onAddTrack)
            onAddTrack();
    };
    importButton.setWantsKeyboardFocus (false);
    importButton.onClick = [this] {
        if (onImport)
            onImport();
    };
    emptyLabel.setText ("No Tracks yet. Add a Track to record on, or import a Backing Track.", juce::dontSendNotification);
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

    cameraArmButton.setClickingTogglesState (false);
    cameraArmButton.setColour (juce::TextButton::buttonOnColourId, colours::rec);
    cameraArmButton.setWantsKeyboardFocus (false);
    cameraArmButton.onClick = [this] {
        if (onCameraArm)
            onCameraArm (! shownCamera.armed);
    };
    cameraPicker.setTextWhenNothingSelected ("Camera?");
    cameraPicker.setTextWhenNoChoicesAvailable ("No camera");
    cameraPicker.setWantsKeyboardFocus (false);
    cameraPicker.onChange = [this] {
        const auto index = cameraPicker.getSelectedItemIndex();
        if (index < 0 || (std::size_t) index >= shownCamera.devices.size() || shownCamera.selected == (std::size_t) index)
            return;
        if (onCameraDevice)
            onCameraDevice (shownCamera.devices[(std::size_t) index].id);
    };
    cameraStage.show (nullptr, juce::String::fromUTF8 (cameraStageMessage (false).c_str()));

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

    addAndMakeVisible (addTrackButton);
    addAndMakeVisible (importButton);
    addChildComponent (emptyLabel);
    addAndMakeVisible (preRollCaption);
    addAndMakeVisible (preRollPicker);
    addAndMakeVisible (cameraArmButton);
    addAndMakeVisible (cameraPicker);
    addAndMakeVisible (cameraStage);
    addChildComponent (stageVideo);
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
            row->onMenu = [this, trackId] {
                if (onTrackMenu)
                    onTrackMenu (trackId);
            };
            row->insertChain.actions = &insertActions;
            addAndMakeVisible (*row);
            rows.push_back (std::move (row));
        }
        resized();
    }

    for (std::size_t i = 0; i < rows.size(); ++i)
        rows[i]->show (state.rows[i]);
    addTrackButton.setEnabled (state.canAddTracks);
    importButton.setEnabled (state.canAddTracks);
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

void RecordPage::showStagePlayback (bool playback)
{
    cameraStage.setVisible (! playback);
    stageVideo.setVisible (playback);
}

void RecordPage::showCamera (const CameraControlsState& state, void* previewLayer)
{
    if (state.devices != shownCamera.devices)
    {
        cameraPicker.clear (juce::dontSendNotification);
        for (std::size_t i = 0; i < state.devices.size(); ++i)
            cameraPicker.addItem (juce::String::fromUTF8 (state.devices[i].name.c_str()), (int) i + 1);
    }
    shownCamera = state;
    if (state.selected.has_value())
        cameraPicker.setSelectedItemIndex ((int) *state.selected, juce::dontSendNotification);
    else
        cameraPicker.setSelectedId (0, juce::dontSendNotification);
    cameraPicker.setEnabled (state.canChoose);
    cameraArmButton.setToggleState (state.armed, juce::dontSendNotification);
    cameraArmButton.setEnabled (state.canArm);
    cameraStage.show (previewLayer, juce::String::fromUTF8 (cameraStageMessage (state.armed).c_str()));
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
    cameraStage.setBounds (layout.stage);
    stageVideo.setBounds (layout.stage.reduced (1));
    cameraArmButton.setBounds (layout.cameraArm);
    cameraPicker.setBounds (layout.cameraPicker);
    for (std::size_t i = 0; i < rows.size(); ++i)
        rows[i]->setBounds (layout.rows[i]);
    addTrackButton.setBounds (layout.addTrackButton);
    importButton.setBounds (layout.importButton);
    emptyLabel.setBounds (layout.addTrackButton.withWidth (trackColumnWidth).translated (0, layout.addTrackButton.getHeight() + 6));
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
}
} // namespace overdub::ui
