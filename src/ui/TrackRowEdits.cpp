#include "ui/TrackRowEdits.h"

#include "song/Separation.h"

#include <algorithm>

namespace overdub::ui
{
std::vector<TrackMenuItem> trackMenuFor (const song::Song& song, const song::Id& trackId, bool passRunning)
{
    const auto separable = song::separableBackingTrack (song, trackId).has_value();
    return { TrackMenuItem { TrackMenuAction::separate, "Separate...", separable && ! passRunning } };
}

std::vector<InputChoice> inputChoices (const std::vector<std::string>& inputChannelNames)
{
    std::vector<InputChoice> choices { InputChoice { {}, "No input" } };
    const auto count = (int) inputChannelNames.size();
    for (int channel = 0; channel < count; ++channel)
        choices.push_back (InputChoice { { channel }, inputChannelNames[(std::size_t) channel] });
    for (int left = 0; left + 1 < count; left += 2)
        choices.push_back (InputChoice { { left, left + 1 }, inputChannelNames[(std::size_t) left] + " + " + inputChannelNames[(std::size_t) left + 1] });
    return choices;
}

std::optional<std::size_t> choiceIndexFor (const std::vector<InputChoice>& choices, const std::vector<int>& channels)
{
    const auto found = std::ranges::find (choices, channels, &InputChoice::channels);
    if (found == choices.end())
        return std::nullopt;
    return (std::size_t) std::distance (choices.begin(), found);
}

song::Song withArmed (song::Song song, const song::Id& trackId, bool armed)
{
    if (auto track = std::ranges::find (song.tracks, trackId, &song::Track::id); track != song.tracks.end())
        track->armed = armed;
    return song;
}

song::Song withInputChannels (song::Song song, const song::Id& trackId, std::vector<int> inputChannels)
{
    if (auto track = std::ranges::find (song.tracks, trackId, &song::Track::id); track != song.tracks.end())
        track->inputChannels = std::move (inputChannels);
    return song;
}

song::Song songForEngine (song::Song song, const std::set<song::Id>& monitoredTrackIds)
{
    for (auto& track : song.tracks)
        track.armed = track.armed && monitoredTrackIds.contains (track.id);
    return song;
}

int nextPassNumber (const song::Song& song)
{
    return (int) song.passes.size() + 1;
}
} // namespace overdub::ui
