#include "video/VideoPlayback.h"

#include "video/SyntheticMovie.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

// VideoPlayback over synthetic movies in a package folder (issue #36): the frame chosen is
// decoded, a movie that cannot be read is logged once and never shown.
using namespace overdub::video;
using overdub::core::ScopedLogCapture;

namespace
{
struct PackageFolder
{
    juce::File root = juce::File::getSpecialLocation (juce::File::tempDirectory)
                          .getChildFile ("overdub-playback-test-" + juce::Uuid().toString());

    PackageFolder() { root.createDirectory(); }
    ~PackageFolder() { root.deleteRecursively(); }
};

int eventCount (const ScopedLogCapture& capture, const std::string& name)
{
    int count = 0;
    for (const auto& event : capture.events())
        if (event.value ("event", "") == name)
            ++count;
    return count;
}
} // namespace

TEST_CASE ("playback decodes the frame chosen for a heard sample, and its span covers the movie", "[video][playback]")
{
    PackageFolder folder;
    auto song = overdub::song::makeEmptySong (48000);
    const auto id = testing::addSyntheticVideoTake (song, folder.root, 48'000, 60); // 2 s from 1 s

    VideoPlayback playback (folder.root);
    const auto spans = playback.spans (song);
    REQUIRE (spans.size() == 1);
    CHECK (spans[0].start == 48'000);
    CHECK (spans[0].end == 48'000 + 96'000);

    const auto shown = playback.frameAt (song, 48'000.0 + 0.5 * 48'000.0 + 10.0);
    CHECK (shown.choice.videoTakeId == id);
    CHECK (shown.choice.frameIndex == std::optional<std::size_t> (15));
    CHECK (shown.sampleBuffer != nullptr);

    const auto none = playback.frameAt (song, 10.0);
    CHECK_FALSE (none.choice.videoTakeId.has_value());
    CHECK (none.sampleBuffer == nullptr);
}

TEST_CASE ("a Video Take whose movie cannot be read is logged once and never shown", "[video][playback]")
{
    PackageFolder folder;
    auto song = overdub::song::makeEmptySong (48000);
    song.passes.push_back ({ "pass-1", 0, "2026-10-08T12:00:00Z" });
    song.videoTakes.push_back ({ "lost", "pass-1", "video/lost.mov", 0, 48000.0, "camera", 0 });
    REQUIRE (folder.root.getChildFile ("video/lost.mov").create().wasOk());

    ScopedLogCapture capture;
    VideoPlayback playback (folder.root);
    for (int i = 0; i < 3; ++i)
        CHECK_FALSE (playback.frameAt (song, 100.0).choice.videoTakeId.has_value());
    CHECK (eventCount (capture, "video.movie_unreadable") == 1);
    REQUIRE (playback.spans (song).size() == 1);
    CHECK_FALSE (playback.spans (song)[0].readable);
}
