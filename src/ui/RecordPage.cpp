#include "ui/RecordPage.h"

#include "core/Log.h"
#include "ui/Colours.h"
#include "ui/Layout.h"
#include "ui/MixerGeometry.h"
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
    state.canChangeMix = ! passRunning;
    const auto choices = inputChoices (inputChannelNames);
    for (const auto& track : song.tracks)
    {
        state.rows.push_back (TrackRowState {
            .trackId = track.id,
            .name = track.name,
            .colour = trackColour (track),
            .armed = track.armed,
            .mute = track.mute,
            .solo = track.solo,
            .choices = choices,
            .selectedChoice = choiceIndexFor (choices, track.inputChannels),
            .monitoring = isMonitoring (track.id),
            .volumeDb = track.volumeDb,
            .inserts = insertChipsFor (track.insertChain, insertStatus),
            .activeInserts = activeInsertCount (track.insertChain, insertStatus),
            .enabled = ! passRunning,
        });
    }
    return state;
}

//==============================================================================

void ClipIndicator::light()
{
    if (lit)
        return;
    lit = true;
    repaint();
}

void ClipIndicator::clear()
{
    if (! lit)
        return;
    lit = false;
    repaint();
}

void ClipIndicator::mouseDown (const juce::MouseEvent&)
{
    if (onClick)
        onClick();
}

void ClipIndicator::paint (juce::Graphics& g)
{
    g.setColour (lit ? colours::rec : colours::background.darker (0.4f));
    g.fillEllipse (getLocalBounds().toFloat());
}

//==============================================================================

TrackRow::TrackRow()
{
    armButton.setColour (juce::TextButton::buttonOnColourId, colours::rec);
    armButton.setTooltip ("Arm");
    monitoringButton.setColour (juce::TextButton::buttonOnColourId, colours::accent);
    setUpMuteSoloButtons (muteButton, soloButton, [this] { return current.mute; }, [this] { return current.solo; }, onMute, onSolo);
    nameLabel.setColour (juce::Label::textColourId, colours::text);
    nameLabel.setFont (juce::FontOptions (14.0f));
    inputPicker.setTextWhenNothingSelected ("Input?");
    volumeLabel.setColour (juce::Label::textColourId, colours::dimText);
    volumeLabel.setFont (juce::FontOptions (11.0f));
    volumeLabel.setJustificationType (juce::Justification::centredRight);
    volumeLabel.setInterceptsMouseClicks (false, false);

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

    fader.onGrab = [this] {
        if (onVolumeGrab)
            onVolumeGrab();
    };
    fader.onDrag = [this] (double db) {
        showVolume (db);
        if (onVolumeDrag)
            onVolumeDrag (db);
    };
    fader.onRelease = [this] (double startDb, double endDb) {
        showVolume (endDb);
        if (juce::exactlyEqual (startDb, endDb))
        {
            if (onVolumeAbandoned)
                onVolumeAbandoned();
        }
        else if (onVolumeChanged)
            onVolumeChanged (endDb);
    };
    fader.onReset = [this] {
        if (onVolumeReset)
            onVolumeReset();
    };

    for (auto* button : { &armButton, &monitoringButton })
        button->setWantsKeyboardFocus (false);
    inputPicker.setWantsKeyboardFocus (false);

    addAndMakeVisible (grip);
    addAndMakeVisible (armButton);
    nameLabel.setInterceptsMouseClicks (false, false); // a click on the name is the row's
    addAndMakeVisible (nameLabel);
    addAndMakeVisible (muteButton);
    addAndMakeVisible (soloButton);
    addAndMakeVisible (inputPicker);
    addAndMakeVisible (monitoringButton);
    addAndMakeVisible (insertChain);
    addAndMakeVisible (fader);
    addAndMakeVisible (volumeLabel);
    addAndMakeVisible (meter);
    addAndMakeVisible (clip);
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

    for (auto* control : std::initializer_list<juce::Component*> { &grip, &armButton, &muteButton, &soloButton, &inputPicker, &monitoringButton, &fader })
        control->setEnabled (state.enabled);
    showVolume (state.volumeDb);
    repaint();
}

void TrackRow::showVolume (double db)
{
    fader.setDb (db); // a drag in progress keeps its own value
    volumeLabel.setText (juce::String (volumeText (fader.db())), juce::dontSendNotification);
}

void TrackRow::showMeters (const engine::MeterPeaks& peaks)
{
    const auto& source = current.armed ? peaks.inputs : peaks.tracks;
    const auto found = source.find (current.trackId);
    const auto peak = found != source.end() ? found->second : 0.0f;
    meter.addPeak (peak);
    if (current.armed && peak >= clipPeak)
        clip.light();
}

void TrackRow::resized()
{
    const auto layout = trackRowLayout (getLocalBounds());
    grip.setBounds (layout.grip);
    armButton.setBounds (layout.arm);
    nameLabel.setBounds (layout.name);
    muteButton.setBounds (layout.mute);
    soloButton.setBounds (layout.solo);
    inputPicker.setBounds (layout.input);
    monitoringButton.setBounds (layout.monitoring);
    insertChain.setBounds (layout.insertChain);
    fader.setBounds (layout.volumeFader);
    volumeLabel.setBounds (layout.volumeText);
    meter.setBounds (layout.volumeMeter);
    clip.setBounds (layout.clipIndicator);
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
    g.setColour (current.colour);
    g.fillRoundedRectangle (area.withTrimmedLeft (1.0f).withWidth ((float) trackColourStripeWidth).reduced (0.0f, 6.0f), 2.0f);
    g.setColour (current.armed ? colours::rec : colours::line);
    g.drawRoundedRectangle (area, 7.0f, 1.0f);
}

//==============================================================================

RecordPage::RecordPage (PeaksLookup peaks)
    : peaksFor (std::move (peaks))
{
    addTrackButton.setWantsKeyboardFocus (false);
    addTrackButton.onClick = [this] { emit (SongIntent { intent::AddTrack {} }); };
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
        emit (SongIntent { intent::SetPreRoll { preRollChoices[(std::size_t) index] } });
    };

    cameraArmButton.setClickingTogglesState (false);
    cameraArmButton.setColour (juce::TextButton::buttonOnColourId, colours::rec);
    cameraArmButton.setWantsKeyboardFocus (false);
    cameraArmButton.onClick = [this] { emit (SongIntent { intent::SetCameraArmed { ! shownCamera.armed } }); };
    cameraPicker.setTextWhenNothingSelected ("Camera?");
    cameraPicker.setTextWhenNoChoicesAvailable ("No camera");
    cameraPicker.setWantsKeyboardFocus (false);
    cameraPicker.onChange = [this] {
        const auto index = cameraPicker.getSelectedItemIndex();
        if (index < 0 || (std::size_t) index >= shownCamera.devices.size() || shownCamera.selected == (std::size_t) index)
            return;
        emit (SongIntent { intent::SetCameraDevice { shownCamera.devices[(std::size_t) index].id } });
    };
    cameraStage.show (nullptr, juce::String::fromUTF8 (cameraStageMessage (false).c_str()));

    passesList.onSelect = [this] (const song::Id& passId) { emit (SongIntent { intent::JumpToPass { passId } }); };
    passesList.onBringBack = [this] (const song::Id& passId) { emit (SongIntent { intent::Edit { intent::BringBackPass { passId } } }); };
    overview.onSeek = [this] (song::SampleCount sample) { emit (SongIntent { intent::Seek { sample } }); };
    mix.onEdit = [this] (const intent::Edit& mixEdit) { emit (SongIntent { mixEdit }); };
    mix.onPreview = [this] (const intent::PreviewMix& preview) { emit (SongIntent { preview }); };
    mix.onFocusChanged = [this] { showFocus(); };
    mix.onKeyValue = [this] (const MixerControl& control, double value) {
        if (auto* row = rowFor (control.trackId))
            row->showVolume (value);
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

void RecordPage::show (const RecordPageState& state, const song::Song& song)
{
    mix.show (song, state.canChangeMix);
    const auto sameTracks = rows.size() == state.rows.size()
                         && std::equal (rows.begin(), rows.end(), state.rows.begin(), [] (const auto& row, const auto& rowState) { return row->trackId() == rowState.trackId; });
    if (! sameTracks)
    {
        // Rows are kept by Track id, so a reorder moves them (with their meters and clip
        // indicators), and the grip whose drop reordered the Song survives its own gesture.
        auto previous = std::move (rows);
        rows.clear();
        for (const auto& rowState : state.rows)
        {
            const auto kept = std::ranges::find_if (previous, [&rowState] (const auto& row) { return row != nullptr && row->trackId() == rowState.trackId; });
            if (kept != previous.end())
            {
                rows.push_back (std::move (*kept));
                continue;
            }
            auto row = std::make_unique<TrackRow>();
            connectRow (*row, rowState.trackId);
            row->insertChain.actions = &insertActions;
            addAndMakeVisible (*row);
            rows.push_back (std::move (row));
        }
        resized();
    }

    for (std::size_t i = 0; i < rows.size(); ++i)
    {
        auto rowState = state.rows[i];
        rowState.volumeDb = mix.shownValue ({ rowState.trackId, MixerControlKind::volume }, rowState.volumeDb);
        rows[i]->show (rowState);
    }
    showFocus();
    rowsEnabled = std::ranges::all_of (state.rows, &TrackRowState::enabled);
    if (! rowsEnabled && cancelTrackDrag())
        core::logEvent (core::Level::debug, "ui.track_drag_cancelled", { { "page", "record" }, { "reason", "pass_running" } });
    addTrackButton.setEnabled (state.canAddTracks);
    importButton.setEnabled (state.canAddTracks);
    emptyLabel.setVisible (rows.empty());

    shownPreRoll = state.preRoll;
    preRollPicker.setSelectedItemIndex (preRollChoiceIndex (state.preRoll), juce::dontSendNotification);
    preRollPicker.setEnabled (state.canChangePreRoll);
}

void RecordPage::showMeters (const engine::MeterPeaks& peaks)
{
    for (auto& row : rows)
        row->showMeters (peaks);
}

void RecordPage::clearClipIndicators()
{
    for (auto& row : rows)
        row->clip.clear();
}

void RecordPage::connectRow (TrackRow& row, const song::Id& trackId)
{
    row.onArm = [this, trackId] (bool armed) { emit (SongIntent { intent::SetArmed { trackId, armed } }); };
    row.onInput = [this, trackId] (std::vector<int> channels) { emit (SongIntent { intent::SetInputChannels { trackId, std::move (channels) } }); };
    row.onMonitoring = [this, trackId] (bool on) { emit (SongIntent { intent::SetMonitoring { trackId, on } }); };
    row.onMute = [this, trackId] (bool mute) { emit (SongIntent { intent::Edit { intent::SetTrackMute { trackId, mute } } }); };
    row.onSolo = [this, trackId] (bool solo) { emit (SongIntent { intent::Edit { intent::SetTrackSolo { trackId, solo } } }); };
    row.clip.onClick = [this, trackId] { emit (SongIntent { intent::ClearClipIndicator { trackId } }); };
    row.onMenu = [this, trackId] {
        if (onTrackMenu)
            onTrackMenu (trackId);
    };
    row.grip.onPress = [this, &row] (juce::Point<int> inGrip) { beginTrackDrag (row, inGrip); };
    row.grip.onDrag = [this, &row] (juce::Point<int> inGrip) { dragTrack (row, inGrip); };
    row.grip.onRelease = [this, &row] (juce::Point<int> inGrip) { dropTrack (row, inGrip); };
    const MixerControl volume { trackId, MixerControlKind::volume };
    row.onVolumeGrab = [this, volume] { mix.focus (volume); };
    row.onVolumeDrag = [this, volume] (double db) { mix.drag (volume, db); };
    row.onVolumeChanged = [this, volume] (double db) { mix.set (volume, db); };
    row.onVolumeAbandoned = [this] { mix.abandon(); };
    row.onVolumeReset = [this, volume] { mix.set (volume, 0.0); };
}

std::vector<juce::Range<int>> RecordPage::trackRowSpans() const
{
    std::vector<juce::Range<int>> spans;
    for (const auto& row : rows)
        spans.push_back (row->getBounds().getVerticalRange());
    return spans;
}

void RecordPage::beginTrackDrag (TrackRow& row, juce::Point<int> inGrip)
{
    const auto found = std::ranges::find_if (rows, [&row] (const auto& candidate) { return candidate.get() == &row; });
    if (! rowsEnabled || found == rows.end())
        return;
    const auto from = (std::size_t) (found - rows.begin());
    const auto pointerY = getLocalPoint (&row.grip, inGrip).y;
    activeTrackDrag.begin (row.trackId(), from, row.getBounds(), row.createComponentSnapshot (row.getLocalBounds()), pointerY, trackRowSpans());
    core::logEvent (core::Level::debug, "ui.track_drag_started", { { "page", "record" }, { "track_id", row.trackId() }, { "from", from } });
    repaint();
}

void RecordPage::dragTrack (TrackRow& row, juce::Point<int> inGrip)
{
    if (! activeTrackDrag.isActive())
        return;
    activeTrackDrag.moveTo (getLocalPoint (&row.grip, inGrip).y, trackRowSpans());
    repaint();
}

void RecordPage::dropTrack (TrackRow& row, juce::Point<int> inGrip)
{
    const auto drop = activeTrackDrag.end (getLocalPoint (&row.grip, inGrip).y, trackRowSpans());
    repaint();
    if (! drop.has_value())
        return;
    core::logEvent (core::Level::debug, "ui.track_dropped", { { "page", "record" }, { "track_id", drop->trackId }, { "from", drop->from }, { "to", drop->to } });
    if (drop->to != drop->from)
        emit (SongIntent { intent::MoveTrack { drop->trackId, drop->to } });
}

bool RecordPage::cancelTrackDrag()
{
    if (! activeTrackDrag.cancel())
        return false;
    repaint();
    return true;
}

void RecordPage::emit (Intent intent) const
{
    if (onIntent)
        onIntent (intent);
}

TrackRow* RecordPage::rowFor (const song::Id& trackId)
{
    for (auto& row : rows)
        if (row->trackId() == trackId)
            return row.get();
    return nullptr;
}

bool RecordPage::isFocusedControl (const juce::Component* component)
{
    const auto& focused = mix.focusedControl();
    if (! focused.has_value() || component == nullptr)
        return false;
    auto* row = rowFor (focused->trackId);
    return row != nullptr && (component == &row->fader || row->fader.isParentOf (component));
}

void RecordPage::showFocus()
{
    const auto& focused = mix.focusedControl();
    for (auto& row : rows)
        row->fader.setFocusShown (focused == MixerControl { row->trackId(), MixerControlKind::volume });
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
        envelope = backingEnvelope (song, peaksFor);
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

void RecordPage::paintOverChildren (juce::Graphics& g)
{
    if (rows.empty())
        return;
    const auto column = rows.front()->getBounds();
    activeTrackDrag.paint (g, column.getX(), column.getRight());
}
} // namespace overdub::ui
