#pragma once

#include "plugin/InsertHost.h"
#include "song/Song.h"
#include "ui/InsertChain.h"
#include "ui/LookAheadStrip.h"
#include "ui/PassesPanel.h"
#include "ui/RecordView.h"
#include "ui/SongOverview.h"
#include "ui/TrackRowEdits.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <array>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

// Record mode (issues #27, #32, issue #9 resolution, variant C), the performing view. Left: the
// pre-roll picker (Off / 1 s / 2 s / 4 s) over the camera stage (a placeholder until slice 14).
// Right column: one row per Track (arm, name, M and S (issue #31), input picker, Monitoring), the
// Import button, then the Passes list (heard status and ↺ Bring back; clicking a row jumps to the
// Pass's start). Along the bottom: the Backing Track look-ahead strip and the whole-Song overview
// (a click moves the playhead). Under each row's name: the Track's Insert Chain (issue #34,
// ui/InsertChain.h: add, open, bypass, remove, drag to reorder); the Monitoring button counts
// the chain's active Inserts. Meters come later.
// Thin: it shows state and reports gestures by Track or Pass id; the strip, overview and Passes
// rows are computed by ui/RecordView.h and ui/TakeLanes.h.
namespace overdub::ui
{
struct TrackRowState
{
    song::Id trackId;
    std::string name;
    bool armed = false;
    bool mute = false;
    bool solo = false;
    std::vector<InputChoice> choices;
    std::optional<std::size_t> selectedChoice; // nullopt: the Track's channels are not offered
    bool monitoring = false;
    std::vector<InsertChipState> inserts;
    int activeInserts = 0; // activeInsertCount of the Track's chain
    bool enabled = true; // false while a Pass runs
};

struct RecordPageState
{
    std::vector<TrackRowState> rows;
    bool canImport = true;
    song::PreRoll preRoll = song::PreRoll::off;
    bool canChangePreRoll = true; // false while a Pass runs
};

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
    void resized() override;
    void paint (juce::Graphics& g) override;

    const song::Id& trackId() const { return current.trackId; }

    std::function<void (bool armed)> onArm;
    std::function<void (std::vector<int> channels)> onInput;
    std::function<void (bool on)> onMonitoring;
    std::function<void (bool mute)> onMute;
    std::function<void (bool solo)> onSolo;

    juce::TextButton armButton { "Arm" };
    juce::Label nameLabel;
    juce::TextButton muteButton { "M" };
    juce::TextButton soloButton { "S" };
    juce::ComboBox inputPicker;
    juce::TextButton monitoringButton { "Monitoring" };
    InsertChainView insertChain { false };

private:
    TrackRowState current;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (TrackRow)
};

class RecordPage final : public juce::Component
{
public:
    RecordPage();

    void show (const RecordPageState& state);
    // The Passes list, the look-ahead strip and the overview for this Song and transport. The
    // Passes rows and the strip's envelope are recomputed only when the Song's Tracks, Passes,
    // Takes or `editable` changed.
    void showSong (const song::Song& song, const RecordTransport& transport, bool editable);
    void resized() override;
    void paint (juce::Graphics& g) override;

    std::function<void (const song::Id&, bool)> onArm;
    std::function<void (const song::Id&, std::vector<int>)> onInput;
    std::function<void (const song::Id&, bool)> onMonitoring;
    std::function<void (const song::Id&, bool)> onMute;
    std::function<void (const song::Id&, bool)> onSolo;
    // The Insert Chains' gestures (every row's).
    InsertChainActions insertActions;
    std::function<void()> onImport;
    std::function<void (song::PreRoll)> onPreRoll;
    // A click on a Pass row: jump to that Pass's start.
    std::function<void (const song::Id& passId)> onJumpToPass;
    std::function<void (const song::Id& passId)> onBringBack;
    // A click on the overview, at this Song sample.
    std::function<void (song::SampleCount sample)> onSeek;
    // The Backing Tracks' peaks, for the strip's envelope (OpenSong::peaks).
    PeaksLookup peaksFor;

    std::vector<std::unique_ptr<TrackRow>> rows;
    juce::TextButton importButton { "Import..." };
    juce::Label emptyLabel;
    juce::Label preRollCaption { {}, "Pre-roll" };
    juce::ComboBox preRollPicker;
    PassesPanel passesList { "Passes \xC2\xB7 newest first \xC2\xB7 click one to jump to its start" };
    LookAheadStrip lookAheadStrip;
    SongOverview overview;

private:
    void showLookAhead();

    juce::Rectangle<int> stageArea;
    song::PreRoll shownPreRoll = song::PreRoll::off;

    // What showSong last showed, to recompute only on change.
    std::optional<song::Song> shownSong;
    bool shownEditable = true;
    BackingEnvelope envelope;
    RecordTransport transport;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (RecordPage)
};
} // namespace overdub::ui
