#pragma once

#include "capture/Camera.h"
#include "plugin/InsertHost.h"
#include "song/Song.h"
#include "ui/CameraStage.h"
#include "ui/InsertChain.h"
#include "ui/Intent.h"
#include "ui/LookAheadStrip.h"
#include "ui/MarkerStrip.h"
#include "ui/MixFocus.h"
#include "ui/Mixer.h"
#include "ui/PassesPanel.h"
#include "ui/PeaksLookup.h"
#include "ui/RecordView.h"
#include "ui/SongOverview.h"
#include "ui/TrackDrag.h"
#include "ui/TrackRowEdits.h"
#include "ui/VideoScreen.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <array>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

// Record mode (issues #27, #32, #35, issue #9 resolution, variant C), the performing view. Left: the
// pre-roll picker (Off / 1 s / 2 s / 4 s), the camera arm and the camera picker over the camera
// stage. The stage shows the camera's live feed (ui/CameraStage.h) while the camera is Armed and a
// Pass runs or the transport is stopped (to frame the shot); otherwise it shows Video Take
// playback (issue #36, ui/VideoScreen.h, stageShowsLiveFeed).
// Right column: one row per Track (grip, name, A, M and S (issue #31), input picker, Monitoring), the
// Import button, then the Passes list (heard status and ↺ Bring back; clicking a row jumps to the
// Pass's start). Along the bottom: the Backing Track look-ahead strip and the whole-Song overview
// (a click moves the playhead), with the read-only Loop/Marker strip on the overview's scale over
// it (issue #227: clicking a Marker runs select_marker, which moves the record point there). Under each row's name: the Track's Insert Chain (issue #34,
// ui/InsertChain.h: add, open, bypass, remove, drag to reorder); the Monitoring button counts
// the chain's active Inserts. A right-click (or control-click) on a row asks for its Track's menu
// (issue #114: Separate...). Along each row's bottom: the Track's horizontal volume fader and its dB
// text (issue #130), the same volume as its Mixer fader and behaving exactly like it (MixFocus:
// drags, detent, double-click, keyboard focus, arrow keys and their undo steps); dimmed and inert
// while a Pass runs. Dragging a row's grip reorders the Tracks (issue #138, ui/TrackDrag.h); a drop
// that changes the order is move_track, and no grip drags while a Pass runs.
// Under the fader, a level meter on the fader's scale (issue #131): an Armed
// Track's raw input (MeterPeaks::inputs, before its Insert Chain and fader, so the fader does not
// move it), any other Track's post-fader output (as its Mixer meter); arming or disarming switches
// it. At the meter's right end the clip indicator (ClipIndicator).
// Thin: it shows state, and every gesture that changes state is an Intent reported through
// onIntent (ADR 0008). Its other hooks ask the user first and run an Intent with the answer:
// Import... (import_files), a row's menu (start_separation) and "+ Insert" (add_insert). The strip,
// overview and Passes rows are computed by ui/RecordView.h and ui/TakeLanes.h.
namespace overdub::ui
{
// A Record row's clip indicator (issue #131): an Armed Track's input peak at or over clipPeak
// (ui/MixerGeometry.h) lights it, and it stays lit until clicked (onClick: clear_clip_indicator) or
// until the next Pass starts. Transient UI state, never in the Song.
class ClipIndicator final : public juce::Component
{
public:
    void light();
    void clear();
    bool isLit() const { return lit; }

    std::function<void()> onClick;

    void mouseDown (const juce::MouseEvent& event) override;
    void paint (juce::Graphics& g) override;

private:
    bool lit = false;
};

struct TrackRowState
{
    song::Id trackId;
    std::string name;
    juce::Colour colour; // the Track's (ui/Colours.h): a stripe along the row's left edge
    bool armed = false;
    bool mute = false;
    bool solo = false;
    std::vector<InputChoice> choices;
    std::optional<std::size_t> selectedChoice; // nullopt: the Track's channels are not offered
    bool monitoring = false;
    double volumeDb = 0.0;
    std::vector<InsertChipState> inserts;
    int activeInserts = 0; // activeInsertCount of the Track's chain
    bool enabled = true; // false while a Pass runs
};

struct RecordPageState
{
    std::vector<TrackRowState> rows;
    bool canAddTracks = true; // Add Track and Import; false while a Pass runs
    song::PreRoll preRoll = song::PreRoll::off;
    bool canChangePreRoll = true; // false while a Pass runs
    bool canChangeMix = true;     // the volume faders; false while a Pass runs
};

// The camera arm and picker over the stage (issue #35).
struct CameraControlsState
{
    bool armed = false;
    std::vector<capture::CameraDevice> devices;
    std::optional<std::size_t> selected; // nullopt: no choice, or the chosen camera is not here
    bool canArm = true;                  // false while a Pass runs, or with no camera to arm
    bool canChoose = true;               // false while a Pass runs

    bool operator== (const CameraControlsState&) const = default;
};

CameraControlsState cameraControlsStateFor (const song::Song& song, const std::vector<capture::CameraDevice>& cameras, bool passRunning);

// The stage's line when it shows no feed.
std::string cameraStageMessage (bool armed);

// Whether the Record stage shows the camera's live feed (true) or Video Take playback (false,
// issue #36): the feed while the camera is Armed and a Pass runs or the transport is stopped.
bool stageShowsLiveFeed (bool cameraArmed, bool passRunning, bool playing);

// The pre-roll picker's items in order, and the item for a setting.
inline constexpr std::array<song::PreRoll, 4> preRollChoices { song::PreRoll::off, song::PreRoll::oneSecond, song::PreRoll::twoSeconds, song::PreRoll::fourSeconds };
int preRollChoiceIndex (song::PreRoll preRoll);

// The Monitoring button's text: "Monitoring", then " · 2 fx" when Inserts are active.
std::string monitoringText (int activeInserts);

// The rows for a Song: one per Track, in Song order; the Song's pre-roll. Without a status lookup
// every Insert shows as ready.
RecordPageState recordPageStateFor (const song::Song& song,
                                    const std::vector<std::string>& inputChannelNames,
                                    const std::function<bool (const song::Id&)>& isMonitoring,
                                    bool passRunning,
                                    const InsertStatusLookup& insertStatus = {});

class TrackRow final : public juce::Component
{
public:
    TrackRow();

    void show (const TrackRowState& state);
    // Shows a volume a drag or key press set, ahead of the Song.
    void showVolume (double db);
    // One poll's peaks (Engine::takeMeterPeaks): the input peak while Armed, else the strip's.
    void showMeters (const engine::MeterPeaks& peaks);
    void resized() override;
    void paint (juce::Graphics& g) override;

    const song::Id& trackId() const { return current.trackId; }
    juce::Colour colour() const { return current.colour; }

    std::function<void (bool armed)> onArm;
    std::function<void (std::vector<int> channels)> onInput;
    std::function<void (bool on)> onMonitoring;
    std::function<void (bool mute)> onMute;
    std::function<void (bool solo)> onSolo;
    // A right-click (or control-click) on the row, outside its controls.
    std::function<void()> onMenu;
    // The volume fader: grabbed, dragged to a dB, released (a release where it started is
    // abandoned), double-clicked.
    std::function<void()> onVolumeGrab;
    std::function<void (double db)> onVolumeDrag;
    std::function<void (double db)> onVolumeChanged;
    std::function<void()> onVolumeAbandoned;
    std::function<void()> onVolumeReset;

    void mouseDown (const juce::MouseEvent& event) override;

    TrackGrip grip;
    juce::TextButton armButton { "A" };
    juce::Label nameLabel;
    juce::TextButton muteButton { "M" };
    juce::TextButton soloButton { "S" };
    juce::ComboBox inputPicker;
    juce::TextButton monitoringButton { "Monitoring" };
    InsertChainView insertChain { false };
    MixerFader fader { MixerFader::Orientation::horizontal };
    juce::Label volumeLabel;
    LevelMeter meter { MixerFader::Orientation::horizontal };
    ClipIndicator clip;

private:
    TrackRowState current;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (TrackRow)
};

class RecordPage final : public juce::Component
{
public:
    explicit RecordPage (PeaksLookup peaksFor);

    // The rows and pickers; `song` is the Song they were made from, whose volumes the faders change.
    void show (const RecordPageState& state, const song::Song& song);
    // The Passes list, the look-ahead strip and the overview for this Song and transport. The
    // Passes rows and the strip's envelope are recomputed only when the Song's Tracks, Passes,
    // Takes or `editable` changed.
    void showSong (const song::Song& song, const RecordTransport& transport, bool editable);
    // The camera arm and picker, and the stage's live feed (a CALayer*, or null).
    void showCamera (const CameraControlsState& state, void* previewLayer);
    // The stage shows the camera (cameraStage) or Video Take playback (stageVideo).
    void showStagePlayback (bool playback);
    bool showsStagePlayback() const { return stageVideo.isVisible(); }
    // One poll's peaks for every row's meter and clip indicator.
    void showMeters (const engine::MeterPeaks& peaks);
    // A Pass started: every row's clip indicator goes out.
    void clearClipIndicators();
    void resized() override;
    void paint (juce::Graphics& g) override;

    // True when `component` is the focused fader or inside it.
    bool isFocusedControl (const juce::Component* component);

    // Where the page's Intents go (MainComponent::run).
    IntentSink onIntent;
    // A row's menu was asked for (issue #114): the window shows it, and its pick runs an Intent.
    std::function<void (const song::Id&)> onTrackMenu;
    // Import...: the window asks for the files, then runs import_files.
    std::function<void()> onImport;
    // "+ Insert" on a row's chain: the window asks for an AU effect, then runs add_insert.
    std::function<void (const song::Id& trackId)> onAddInsert;

    // The grip drag in progress, if any; cancelTrackDrag ends it without a drop and returns whether
    // there was one (Esc).
    const TrackDrag& trackDrag() const { return activeTrackDrag; }
    bool cancelTrackDrag();
    // Each row as a vertical span in this Component's coordinates, in Song order.
    std::vector<juce::Range<int>> trackRowSpans() const;
    void paintOverChildren (juce::Graphics& g) override;

    // The faders' focus, arrow keys and undo steps (their Intents run through the window); the
    // window sends it keys and commit triggers.
    MixFocus mix;
    std::vector<std::unique_ptr<TrackRow>> rows;
    juce::TextButton addTrackButton { "Add Track" };
    juce::TextButton importButton { "Import..." };
    juce::Label emptyLabel;
    juce::Label preRollCaption { {}, "Pre-roll" };
    juce::ComboBox preRollPicker;
    juce::TextButton cameraArmButton { "Camera" };
    juce::ComboBox cameraPicker;
    CameraStage cameraStage;
    VideoScreen stageVideo;
    PassesPanel passesList { "Passes \xC2\xB7 newest first \xC2\xB7 click one to jump to its start" };
    LookAheadStrip lookAheadStrip;
    MarkerStrip markerStrip;
    SongOverview overview;

private:
    void showLookAhead();
    void connectRow (TrackRow& row, const song::Id& trackId);
    void beginTrackDrag (TrackRow& row, juce::Point<int> inGrip);
    void dragTrack (TrackRow& row, juce::Point<int> inGrip);
    void dropTrack (TrackRow& row, juce::Point<int> inGrip);
    TrackRow* rowFor (const song::Id& trackId);
    void showFocus();
    void emit (Intent intent) const;

    PeaksLookup peaksFor;
    song::PreRoll shownPreRoll = song::PreRoll::off;
    TrackDrag activeTrackDrag;
    bool rowsEnabled = true;
    CameraControlsState shownCamera;

    // What showSong last showed, to recompute only on change.
    std::optional<song::Song> shownSong;
    bool shownEditable = true;
    BackingEnvelope envelope;
    RecordTransport transport;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (RecordPage)
};
} // namespace overdub::ui
