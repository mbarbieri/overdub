#include "song/MarkerEdits.h"

#include <algorithm>
#include <cctype>

namespace overdub::song
{
core::Error markerNotFound (const Id& markerId)
{
    return core::Error { "song.marker_not_found", "no Marker with this id in the Song", { { "marker_id", markerId } } };
}

namespace
{

// Inserts the Marker after every Marker at or before its position.
void insertInTimeOrder (std::vector<Marker>& markers, Marker marker)
{
    const auto after = std::ranges::upper_bound (markers, marker.position, {}, &Marker::position);
    markers.insert (after, std::move (marker));
}

std::string trimmed (const std::string& text)
{
    const auto isSpace = [] (unsigned char c) { return std::isspace (c) != 0; };
    const auto first = std::ranges::find_if_not (text, isSpace);
    const auto last = std::ranges::find_if_not (text.rbegin(), text.rend(), isSpace).base();
    return first < last ? std::string (first, last) : std::string {};
}
} // namespace

std::string nextMarkerName (const Song& song)
{
    for (int number = 1;; ++number)
    {
        auto name = "Marker " + std::to_string (number);
        if (std::ranges::find (song.markers, name, &Marker::name) == song.markers.end())
            return name;
    }
}

const Marker* findMarker (const Song& song, const Id& markerId)
{
    const auto found = std::ranges::find (song.markers, markerId, &Marker::id);
    return found == song.markers.end() ? nullptr : &*found;
}

Song addMarker (Song song, SampleCount position, const Id& markerId)
{
    auto name = nextMarkerName (song);
    insertInTimeOrder (song.markers, Marker { .id = markerId, .name = std::move (name), .position = std::max<SampleCount> (0, position) });
    return song;
}

core::Expected<Song> moveMarker (Song song, const Id& markerId, SampleCount position)
{
    const auto found = std::ranges::find (song.markers, markerId, &Marker::id);
    if (found == song.markers.end())
        return std::unexpected (markerNotFound (markerId));
    auto marker = *found;
    song.markers.erase (found);
    marker.position = std::max<SampleCount> (0, position);
    insertInTimeOrder (song.markers, std::move (marker));
    return song;
}

core::Expected<Song> renameMarker (Song song, const Id& markerId, const std::string& name)
{
    const auto found = std::ranges::find (song.markers, markerId, &Marker::id);
    if (found == song.markers.end())
        return std::unexpected (markerNotFound (markerId));
    auto newName = trimmed (name);
    if (newName.empty())
        return std::unexpected (core::Error { "song.marker_name_empty", "a Marker's name may not be empty", { { "marker_id", markerId } } });
    found->name = std::move (newName);
    return song;
}

core::Expected<Song> deleteMarker (Song song, const Id& markerId)
{
    const auto found = std::ranges::find (song.markers, markerId, &Marker::id);
    if (found == song.markers.end())
        return std::unexpected (markerNotFound (markerId));
    song.markers.erase (found);
    return song;
}
} // namespace overdub::song
