#pragma once

#include "song/Song.h"
#include "ui/TrackRowEdits.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

// Bare Record mode (issue #27): one row per Track (arm, name, input picker, Monitoring) and an
// Import button. The camera stage, meters, M/S, Insert Chains, the Passes list and the look-ahead
// strip come in later slices. Thin: rows show a TrackRowState and report gestures by Track id.
namespace overdub::ui
{
struct TrackRowState
{
    song::Id trackId;
    std::string name;
    bool armed = false;
    std::vector<InputChoice> choices;
    std::optional<std::size_t> selectedChoice; // nullopt: the Track's channels are not offered
    bool monitoring = false;
    bool enabled = true; // false while a Pass runs
};

struct RecordPageState
{
    std::vector<TrackRowState> rows;
    bool canImport = true;
};

// The rows for a Song: one per Track, in Song order.
RecordPageState recordPageStateFor (const song::Song& song,
                                    const std::vector<std::string>& inputChannelNames,
                                    const std::function<bool (const song::Id&)>& isMonitoring,
                                    bool passRunning);

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

    juce::TextButton armButton { "Arm" };
    juce::Label nameLabel;
    juce::ComboBox inputPicker;
    juce::TextButton monitoringButton { "Monitoring" };

private:
    TrackRowState current;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (TrackRow)
};

class RecordPage final : public juce::Component
{
public:
    RecordPage();

    void show (const RecordPageState& state);
    void resized() override;
    void paint (juce::Graphics& g) override;

    std::function<void (const song::Id&, bool)> onArm;
    std::function<void (const song::Id&, std::vector<int>)> onInput;
    std::function<void (const song::Id&, bool)> onMonitoring;
    std::function<void()> onImport;

    std::vector<std::unique_ptr<TrackRow>> rows;
    juce::TextButton importButton { "Import..." };
    juce::Label emptyLabel;

private:
    juce::Rectangle<int> stageArea;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (RecordPage)
};
} // namespace overdub::ui
