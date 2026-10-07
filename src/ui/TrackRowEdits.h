#pragma once

#include "song/Song.h"

#include <cstddef>
#include <optional>
#include <set>
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

// The Song as the engine should render it: Monitoring is per Track and transient (not in the
// Song), and the engine monitors every Armed Track (engine/TrackPlayer.h), so a Track whose
// Monitoring is off is handed to the engine as not Armed. Recording reads the package's Song, not
// this one, so it still records every Armed Track.
song::Song songForEngine (song::Song song, const std::set<song::Id>& monitoredTrackIds);

// The number the next Pass gets: one more than the Song's Passes ("Record Pass <n>").
int nextPassNumber (const song::Song& song);
} // namespace overdub::ui
