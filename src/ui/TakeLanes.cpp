#include "ui/TakeLanes.h"

#include "ui/EditPage.h"

#include <format>
#include <ranges>

namespace overdub::ui
{
std::vector<TakeLaneRow> takeLaneRows (const song::Song& song, const song::Id& trackId)
{
    std::vector<TakeLaneRow> rows;
    for (const auto* take : song::laneTakes (song, trackId))
    {
        TakeLaneRow row;
        row.takeId = take->id;
        const auto number = song::passNumber (song, take->passId);
        row.title = number > 0 ? std::format ("Pass {}", number) : std::string ("Take");
        row.recorded = song::recordedSpan (song, take->id);
        row.heard = song::heardSpans (song, take->id);
        row.status = song::takeHeardStatus (song, take->id);
        rows.push_back (std::move (row));
    }
    return rows;
}

LaneClick laneClickAt (const song::Song& song, const song::Id& takeId, song::SampleCount at)
{
    const auto* take = song::findTake (song, takeId);
    const auto* track = take != nullptr ? song::findTrack (song, take->trackId) : nullptr;
    const auto span = song::recordedSpan (song, takeId);
    if (track == nullptr || ! span.has_value() || at < span->start || at >= span->end)
        return {};

    const auto* top = song::topClipAt (*track, at);
    if (top == nullptr || top->source.kind != song::ClipSourceKind::take)
        return { LaneClickKind::bringBack, {} };
    if (top->source.id == takeId)
        return {};
    return { LaneClickKind::swap, top->id };
}

std::optional<LaneEdit> laneEditAt (const song::Song& song, const song::Id& takeId, song::SampleCount at, const song::Id& newClipId)
{
    const auto click = laneClickAt (song, takeId, at);
    const auto* take = song::findTake (song, takeId);
    if (click.kind == LaneClickKind::none || take == nullptr)
        return std::nullopt;

    // laneClickAt checked the Take has audio at `at`, under the swapped Clip, on a live Track, so
    // neither operation fails here; a failure still changes nothing.
    if (click.kind == LaneClickKind::swap)
    {
        auto swapped = song::swapClipTake (song, click.clipId, takeId);
        if (! swapped.has_value())
            return std::nullopt;
        return LaneEdit { std::move (*swapped), song::useTakeLabel (song, *take), click.clipId };
    }
    auto brought = song::bringBackTake (song, takeId, newClipId);
    if (! brought.has_value())
        return std::nullopt;
    return LaneEdit { std::move (*brought), song::bringBackTakeLabel (song, *take), newClipId };
}

std::vector<PassRow> passRows (const song::Song& song)
{
    std::vector<PassRow> rows;
    for (const auto& pass : song.passes | std::views::reverse)
    {
        std::string trackNames;
        for (const auto& track : song.tracks)
        {
            const auto recordedHere = std::ranges::any_of (song.takes, [&] (const song::Take& take) { return take.passId == pass.id && take.trackId == track.id; });
            if (! recordedHere)
                continue;
            trackNames += (trackNames.empty() ? "" : ", ") + track.name;
        }

        PassRow row;
        row.passId = pass.id;
        row.title = std::format ("Pass {} \xC2\xB7 {} \xC2\xB7 {}",
                                 song::passNumber (song, pass.id),
                                 secondsText (pass.startSample, song.sampleRate),
                                 trackNames.empty() ? std::string ("no Track") : trackNames);
        row.status = song::passHeardStatus (song, pass.id);
        row.canBringBack = song::bringBackPass (song, pass.id).has_value();
        rows.push_back (std::move (row));
    }
    return rows;
}
} // namespace overdub::ui
