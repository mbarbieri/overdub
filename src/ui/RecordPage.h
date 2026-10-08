#pragma once

#include "plugin/InsertHost.h"
#include "song/Song.h"
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
// (a click moves the playhead). Under each row's name: "+ Insert" (issue #33, a minimal way to add
// an AU before the chain editor of slice 13) and the Insert Chain's names. Meters come later.
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
    std::string inserts; // insertChainText of the Track's chain
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

using InsertStatusLookup = std::function<std::optional<plugin::InsertStatus> (const song::Id& slotId)>;

// An Insert Chain as a row shows it: the plugins' names in order, joined by " Â· ", each marked
// " (loading)" or " (missing)" when its plugin is not ready; "No Inserts" for an empty chain.
std::string insertChainText (const std::vector<song::InsertSlot>& chain, const InsertStatusLookup& statusOf);

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
    std::function<void()> onInsert;

    juce::TextButton armButton { "Arm" };
    juce::Label nameLabel;
    juce::TextButton muteButton { "M" };
    juce::TextButton soloButton { "S" };
    juce::ComboBox inputPicker;
    juce::TextButton monitoringButton { "Monitoring" };
    juce::TextButton insertButton { "+ Insert" };
    juce::Label insertsLabel;

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
    // "+ Insert" on a Track's row.
    std::function<void (const song::Id& trackId)> onInsert;
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
