#include "song/LoopEdits.h"

#include "song/PracticeSpeed.h"

#include <algorithm>
#include <cctype>

namespace overdub::song
{
core::Error loopNotFound (const Id& loopId)
{
    return core::Error { "song.loop_not_found", "no Loop with this id in the Song", { { "loop_id", loopId } } };
}

namespace
{
// Inserts the Loop after every Loop that starts at or before its start.
void insertInTimeOrder (std::vector<Loop>& loops, Loop loop)
{
    const auto after = std::ranges::upper_bound (loops, loop.start, {}, &Loop::start);
    loops.insert (after, std::move (loop));
}

// The range between from and to, clamped to 0 or later, or song.loop_empty.
core::Expected<std::pair<SampleCount, SampleCount>> rangeBetween (SampleCount from, SampleCount to)
{
    const auto start = std::max<SampleCount> (0, std::min (from, to));
    const auto end = std::max<SampleCount> (0, std::max (from, to));
    if (end <= start)
        return std::unexpected (core::Error { "song.loop_empty", "a Loop needs a range with a length", { { "start", start }, { "end", end } } });
    return std::pair { start, end };
}

std::string trimmed (const std::string& text)
{
    const auto isSpace = [] (unsigned char c) { return std::isspace (c) != 0; };
    const auto first = std::ranges::find_if_not (text, isSpace);
    const auto last = std::ranges::find_if_not (text.rbegin(), text.rend(), isSpace).base();
    return first < last ? std::string (first, last) : std::string {};
}
} // namespace

std::string nextLoopName (const Song& song)
{
    for (int number = 1;; ++number)
    {
        auto name = "Loop " + std::to_string (number);
        if (std::ranges::find (song.loops, name, &Loop::name) == song.loops.end())
            return name;
    }
}

const Loop* findLoop (const Song& song, const Id& loopId)
{
    const auto found = std::ranges::find (song.loops, loopId, &Loop::id);
    return found == song.loops.end() ? nullptr : &*found;
}

const Loop* activeLoop (const Song& song)
{
    return song.settings.activeLoopId.has_value() ? findLoop (song, *song.settings.activeLoopId) : nullptr;
}

core::Expected<Song> addLoop (Song song, SampleCount from, SampleCount to, const Id& loopId, int practiceSpeedPercent)
{
    const auto range = rangeBetween (from, to);
    if (! range.has_value())
        return std::unexpected (range.error());
    if (auto checked = checkPracticeSpeed (practiceSpeedPercent); ! checked.has_value())
        return std::unexpected (std::move (checked.error()));
    auto name = nextLoopName (song);
    insertInTimeOrder (song.loops, Loop { .id = loopId, .name = std::move (name), .start = range->first, .end = range->second, .settings = { .practiceSpeedPercent = practiceSpeedPercent } });
    return song;
}

core::Expected<Song> resizeLoop (Song song, const Id& loopId, SampleCount from, SampleCount to)
{
    const auto found = std::ranges::find (song.loops, loopId, &Loop::id);
    if (found == song.loops.end())
        return std::unexpected (loopNotFound (loopId));
    const auto range = rangeBetween (from, to);
    if (! range.has_value())
        return std::unexpected (range.error());
    auto loop = *found;
    song.loops.erase (found);
    loop.start = range->first;
    loop.end = range->second;
    insertInTimeOrder (song.loops, std::move (loop));
    return song;
}

core::Expected<Song> renameLoop (Song song, const Id& loopId, const std::string& name)
{
    const auto found = std::ranges::find (song.loops, loopId, &Loop::id);
    if (found == song.loops.end())
        return std::unexpected (loopNotFound (loopId));
    auto newName = trimmed (name);
    if (newName.empty())
        return std::unexpected (core::Error { "song.loop_name_empty", "a Loop's name may not be empty", { { "loop_id", loopId } } });
    found->name = std::move (newName);
    return song;
}

core::Expected<Song> deleteLoop (Song song, const Id& loopId)
{
    const auto found = std::ranges::find (song.loops, loopId, &Loop::id);
    if (found == song.loops.end())
        return std::unexpected (loopNotFound (loopId));
    song.loops.erase (found);
    if (song.settings.activeLoopId == loopId)
    {
        song.settings.activeLoopId.reset();
        song.settings.looping = false;
    }
    if (song.settings.exportLoopId == loopId)
        song.settings.exportLoopId.reset(); // Export goes back to the whole Song (issue #235)
    return song;
}

core::Expected<Song> setExportLoop (Song song, const std::optional<Id>& loopId)
{
    if (loopId.has_value() && findLoop (song, *loopId) == nullptr)
        return std::unexpected (loopNotFound (*loopId));
    song.settings.exportLoopId = loopId;
    return song;
}

core::Expected<Song> setActiveLoop (Song song, const std::optional<Id>& loopId, bool looping)
{
    if (loopId.has_value() && findLoop (song, *loopId) == nullptr)
        return std::unexpected (loopNotFound (*loopId));
    song.settings.activeLoopId = loopId;
    song.settings.looping = loopId.has_value() && looping;
    return song;
}

core::Expected<Song> setLooping (Song song, bool looping)
{
    if (looping && activeLoop (song) == nullptr)
        return std::unexpected (core::Error { "song.no_active_loop", "looping needs an active Loop", {} });
    song.settings.looping = looping;
    return song;
}

core::Expected<Song> setLoopPracticeSpeed (Song song, const Id& loopId, int practiceSpeedPercent)
{
    const auto found = std::ranges::find (song.loops, loopId, &Loop::id);
    if (found == song.loops.end())
        return std::unexpected (loopNotFound (loopId));
    if (auto checked = checkPracticeSpeed (practiceSpeedPercent); ! checked.has_value())
        return std::unexpected (std::move (checked.error()));
    found->settings.practiceSpeedPercent = practiceSpeedPercent;
    return song;
}

std::optional<Id> adjacentLoop (const Song& song, const std::optional<Id>& from, LoopDirection direction)
{
    if (song.loops.empty())
        return std::nullopt;
    const auto found = from.has_value() ? std::ranges::find (song.loops, *from, &Loop::id) : song.loops.end();
    if (found == song.loops.end())
        return direction == LoopDirection::next ? song.loops.front().id : song.loops.back().id;
    if (direction == LoopDirection::next)
        return std::next (found) == song.loops.end() ? std::nullopt : std::optional (std::next (found)->id);
    return found == song.loops.begin() ? std::nullopt : std::optional (std::prev (found)->id);
}
} // namespace overdub::song
