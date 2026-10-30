#pragma once

#include "song/Song.h"

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

// What a Record mode Track row's gestures do to the Song (issue #27), as pure functions the row
// Components call: arm, pick an input, and which Tracks the engine lets you hear live.
// Arm and input are outside undo history (ADR 0004): the caller applies them with
// SongPackage::changeWithoutHistory.
namespace overdub::ui
{
// One entry of a Track row's input picker: the device input channels it records (0-based) and
// its label. Empty channels = "No input".
struct InputChoice
{
    std::vector<int> channels;
    std::string label;

    bool operator== (const InputChoice&) const = default;
};

// "No input", then one mono choice per device input channel (labelled with its name), then one
// stereo choice per consecutive pair 1+2, 3+4, ... ("In 1 + In 2"). An odd last channel has no pair.
std::vector<InputChoice> inputChoices (const std::vector<std::string>& inputChannelNames);

// The index of the choice recording exactly these channels; nullopt when none does (a channel
// this device does not have, or a pair that is not offered).
std::optional<std::size_t> choiceIndexFor (const std::vector<InputChoice>& choices, const std::vector<int>& channels);

// The Song with that Track's Armed flag / input channels set. An unknown Track id changes nothing.
song::Song withArmed (song::Song song, const song::Id& trackId, bool armed);
song::Song withInputChannels (song::Song song, const song::Id& trackId, std::vector<int> inputChannels);
// The Song with the Record page's pre-roll set.
song::Song withPreRoll (song::Song song, song::PreRoll preRoll);

// One item of a Track row's menu (right-click, issue #114).
enum class TrackMenuAction
{
    separate
};

struct TrackMenuItem
{
    TrackMenuAction action = TrackMenuAction::separate;
    std::string label;
    bool enabled = false;
    std::string tooltip; // why a disabled item is disabled; empty when enabled

    bool operator== (const TrackMenuItem&) const = default;
};

// What runs in the open Song that a Track's menu depends on.
struct TrackMenuState
{
    // The code start_separation is refused with now (ui/Availability.h), none when it may run.
    std::optional<std::string> separationRefusal;
};

// A Track's menu: "Separate..." (issue #115), enabled when the Track's Clips all play one Backing
// Track (song::separableBackingTrack) and start_separation may run. Disabled, its tooltip says why:
// "Stop recording first", "Wait for the export to finish", "Wait for what runs to finish" (any
// other refusal), "This Track has no Clips", "This Track
// has recorded Takes", "This Track has more than one Backing Track", "This Track's Backing Track is
// missing".
std::vector<TrackMenuItem> trackMenuFor (const song::Song& song, const song::Id& trackId, TrackMenuState state);

// The number the next Pass gets: one more than the Song's Passes ("Record Pass <n>").
int nextPassNumber (const song::Song& song);
} // namespace overdub::ui
