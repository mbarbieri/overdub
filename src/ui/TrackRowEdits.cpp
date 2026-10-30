#include "ui/TrackRowEdits.h"

#include "song/Separation.h"
#include "ui/Availability.h"

#include <algorithm>

namespace overdub::ui
{
namespace
{
// Why a Track cannot be separated, from song.track_not_separable's reason.
std::string notSeparableTooltip (const core::Error& error)
{
    if (error.code != "song.track_not_separable")
        return "This Track is no longer in the Song";
    const auto reason = error.fields.value ("reason", std::string());
    if (reason == "no_clips")
        return "This Track has no Clips";
    if (reason == "take_clips")
        return "This Track has recorded Takes";
    if (reason == "several_backing_tracks")
        return "This Track has more than one Backing Track";
    return "This Track's Backing Track is missing";
}

std::string separateTooltip (const song::Song& song, const song::Id& trackId, TrackMenuState state)
{
    if (state.separationRefusal.has_value())
        return unavailableReason (*state.separationRefusal);
    const auto separable = song::separableBackingTrack (song, trackId);
    return separable.has_value() ? std::string() : notSeparableTooltip (separable.error());
}
} // namespace

std::vector<TrackMenuItem> trackMenuFor (const song::Song& song, const song::Id& trackId, TrackMenuState state)
{
    auto tooltip = separateTooltip (song, trackId, state);
    const auto enabled = tooltip.empty();
    return { TrackMenuItem { TrackMenuAction::separate, "Separate...", enabled, std::move (tooltip) } };
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
    if (auto* track = song::findTrack (song, trackId))
        track->settings.armed = armed;
    return song;
}

song::Song withInputChannels (song::Song song, const song::Id& trackId, std::vector<int> inputChannels)
{
    if (auto* track = song::findTrack (song, trackId))
        track->settings.inputChannels = std::move (inputChannels);
    return song;
}

int nextPassNumber (const song::Song& song)
{
    return (int) song.passes.size() + 1;
}
} // namespace overdub::ui
