#include "song/VideoTakeEdits.h"

#include <catch2/catch_test_macros.hpp>

// Deleting a Video Take (issue #128): one Video Take leaves the Song, its Pass and the Takes stay,
// and the export choice is cleared only when it named the removed Video Take.
using namespace overdub::song;

namespace
{
Song songWithTwoVideoTakes()
{
    auto song = makeEmptySong (48000);
    song.passes = { Pass { .id = "p1" }, Pass { .id = "p2" } };
    song.takes = { Take { .id = "t1", .passId = "p1" }, Take { .id = "t2", .passId = "p2" } };
    song.videoTakes = { VideoTake { .id = "v1", .passId = "p1", .file = "video/v1.mov" },
                        VideoTake { .id = "v2", .passId = "p2", .file = "video/v2.mov", .startSample = 48000 } };
    return song;
}
} // namespace

TEST_CASE ("deleting a Video Take removes only it, keeping Passes and Takes", "[song][video_takes]")
{
    const auto song = songWithTwoVideoTakes();

    const auto deleted = deleteVideoTake (song, "v1");
    REQUIRE (deleted.has_value());
    REQUIRE (deleted->videoTakes.size() == 1);
    CHECK (deleted->videoTakes[0] == song.videoTakes[1]);
    CHECK (deleted->passes == song.passes);
    CHECK (deleted->takes == song.takes);
    CHECK (std::string (deleteVideoTakeLabel) == "Delete Video Take");
}

TEST_CASE ("deleting the export's chosen Video Take clears the choice; deleting another keeps it", "[song][video_takes]")
{
    auto song = songWithTwoVideoTakes();
    song.settings.exportVideoTakeId = "v2";

    const auto other = deleteVideoTake (song, "v1");
    REQUIRE (other.has_value());
    CHECK (other->settings.exportVideoTakeId == std::optional<Id> ("v2"));

    const auto chosen = deleteVideoTake (song, "v2");
    REQUIRE (chosen.has_value());
    CHECK (! chosen->settings.exportVideoTakeId.has_value());
}

TEST_CASE ("deleting an unknown Video Take is a song.video_take_not_found Error", "[song][video_takes]")
{
    const auto deleted = deleteVideoTake (songWithTwoVideoTakes(), "gone");
    REQUIRE (! deleted.has_value());
    CHECK (deleted.error().code == "song.video_take_not_found");
    CHECK (deleted.error().fields.at ("video_take_id") == "gone");
}
