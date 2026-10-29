#include "song/MarkerEdits.h"

#include "song/MixEdits.h"

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

using namespace overdub::song;

// Markers (issue #227, ADR 0010) as pure Song functions: "Marker N" names, time order, and the
// Errors of a missing Marker or an empty name.
namespace
{
std::vector<std::string> namesInOrder (const Song& song)
{
    std::vector<std::string> names;
    for (const auto& marker : song.markers)
        names.push_back (marker.name);
    return names;
}
} // namespace

TEST_CASE ("a new Marker is named Marker N with the lowest unused N and placed in time order", "[song][markers]")
{
    auto song = makeEmptySong (48000);
    song = addMarker (song, 48000, "m-a");
    song = addMarker (song, 0, "m-b");
    song = addMarker (song, 96000, "m-c");

    CHECK (namesInOrder (song) == std::vector<std::string> { "Marker 2", "Marker 1", "Marker 3" });
    CHECK (song.markers[0].position == 0);
    CHECK (song.markers[1].position == 48000);
    CHECK (song.markers[2].position == 96000);

    // Renaming "Marker 1" frees its number for the next one.
    song = *renameMarker (song, "m-a", "Chorus");
    CHECK (nextMarkerName (song) == "Marker 1");
    song = addMarker (song, 48000, "m-d");
    // At the same position, the new Marker goes after the one already there.
    CHECK (namesInOrder (song) == std::vector<std::string> { "Marker 2", "Chorus", "Marker 1", "Marker 3" });
    CHECK (findMarker (song, "m-d")->name == "Marker 1");
    CHECK (findMarker (song, "m-gone") == nullptr);
}

TEST_CASE ("moving a Marker keeps the Markers in time order and never before the Song's start", "[song][markers]")
{
    auto song = addMarker (addMarker (makeEmptySong (48000), 1000, "m-a"), 5000, "m-b");

    song = *moveMarker (song, "m-a", 9000);
    CHECK (namesInOrder (song) == std::vector<std::string> { "Marker 2", "Marker 1" });
    CHECK (findMarker (song, "m-a")->position == 9000);

    song = *moveMarker (song, "m-a", -50);
    CHECK (findMarker (song, "m-a")->position == 0);
    CHECK (song.markers.front().id == "m-a");

    const auto missing = moveMarker (song, "m-gone", 10);
    REQUIRE_FALSE (missing.has_value());
    CHECK (missing.error().code == "song.marker_not_found");
    CHECK (missing.error().fields.at ("marker_id") == "m-gone");
}

TEST_CASE ("renaming a Marker trims the name and refuses an empty one", "[song][markers]")
{
    const auto song = addMarker (makeEmptySong (48000), 0, "m-a");

    CHECK (renameMarker (song, "m-a", "  Solo  ")->markers.front().name == "Solo");
    const auto empty = renameMarker (song, "m-a", "   ");
    REQUIRE_FALSE (empty.has_value());
    CHECK (empty.error().code == "song.marker_name_empty");
    CHECK (empty.error().fields.at ("marker_id") == "m-a");
    CHECK (renameMarker (song, "m-gone", "Solo").error().code == "song.marker_not_found");
}

TEST_CASE ("deleting a Marker removes only it", "[song][markers]")
{
    const auto song = addMarker (addMarker (makeEmptySong (48000), 0, "m-a"), 10, "m-b");

    const auto deleted = deleteMarker (song, "m-a");
    REQUIRE (deleted.has_value());
    REQUIRE (deleted->markers.size() == 1);
    CHECK (deleted->markers.front().id == "m-b");
    CHECK (deleteMarker (song, "m-gone").error().code == "song.marker_not_found");
}

TEST_CASE ("a Song that differs only in its Markers differs only in the mix the engine hears", "[song][markers]")
{
    const auto song = makeEmptySong (48000);
    CHECK (differsOnlyInMix (song, addMarker (song, 100, "m-a")));
}
