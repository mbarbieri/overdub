#pragma once

#include "core/Error.h"
#include "song/Song.h"

#include <string>

// Markers (ADR 0010, issue #227): labelled points in time on the Song, as pure functions. Each
// change is one undo step with its label (SongPackage::edit). Song::markers stays in time order:
// every function here leaves it sorted by position, Markers at one position in the order they were
// placed or moved there.
namespace overdub::song
{
inline constexpr auto addMarkerLabel = "Add Marker";
inline constexpr auto moveMarkerLabel = "Move Marker";
inline constexpr auto renameMarkerLabel = "Rename Marker";
inline constexpr auto deleteMarkerLabel = "Delete Marker";

// "Marker N" with the lowest N >= 1 that no Marker in the Song is named.
std::string nextMarkerName (const Song& song);

// song.marker_not_found (marker_id): the only place that Error is built.
core::Error markerNotFound (const Id& markerId);

// The Marker with this id, or null.
const Marker* findMarker (const Song& song, const Id& markerId);

// The Song with a new Marker `markerId` named nextMarkerName at `position` (0 or later), after any
// Marker already there.
Song addMarker (Song song, SampleCount position, const Id& markerId);

// The Song with the Marker at `position` (clamped to 0 or later), after any other Marker there.
// Errors: song.marker_not_found (marker_id).
core::Expected<Song> moveMarker (Song song, const Id& markerId, SampleCount position);

// The Song with the Marker named `name` without its leading and trailing whitespace.
// Errors: song.marker_not_found (marker_id), song.marker_name_empty (marker_id): nothing but
// whitespace.
core::Expected<Song> renameMarker (Song song, const Id& markerId, const std::string& name);

// Errors: song.marker_not_found (marker_id).
core::Expected<Song> deleteMarker (Song song, const Id& markerId);
} // namespace overdub::song
