#include "video/FrameSelection.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <map>
#include <vector>

// Frame selection (ADR 0003, issue #36): computed expectations from synthetic anchors and frame
// times, including a camera stall and a seek.
using namespace overdub::video;
using overdub::engine::Anchor;
using overdub::song::SampleCount;

namespace
{
constexpr int sampleRate = 48000;
constexpr std::uint64_t msNs = 1'000'000;
constexpr std::uint64_t refreshNs = 16'666'667; // one 60 Hz refresh

// 30 fps frame times for `frames` frames, with a stall of `stallSeconds` after frame `stallAfter`.
MovieTiming timingOf30Fps (int frames, int stallAfter = -1, double stallSeconds = 0.0)
{
    MovieTiming timing;
    for (int i = 0; i < frames; ++i)
        timing.frameSeconds.push_back (i / 30.0 + (stallAfter >= 0 && i > stallAfter ? stallSeconds : 0.0));
    timing.durationSeconds = timing.frameSeconds.back() + 1.0 / 30.0;
    return timing;
}

struct SongWithVideo
{
    overdub::song::Song song = overdub::song::makeEmptySong (sampleRate);
    std::map<overdub::song::Id, MovieTiming> timings;

    // Adds a Pass and its Video Take starting at `start`, playing at the nominal rate.
    void addVideoTake (const std::string& id, SampleCount start, MovieTiming timing)
    {
        song.passes.push_back ({ "pass-" + id, start, "2026-10-08T12:00:00Z" });
        song.videoTakes.push_back ({ id, "pass-" + id, "video/" + id + ".mov", start, (double) sampleRate, "camera", 0 });
        timings[id] = std::move (timing);
    }

    TimingLookup lookup() const
    {
        return [this] (const overdub::song::VideoTake& videoTake) -> const MovieTiming* {
            const auto found = timings.find (videoTake.id);
            return found != timings.end() ? &found->second : nullptr;
        };
    }
};
} // namespace

TEST_CASE ("the heard Song sample is the latest anchor plus elapsed host time and one refresh, less the output latency", "[video][frames]")
{
    PlaybackClock clock;
    clock.playing = true;
    clock.position = 96'000;
    clock.anchor = Anchor { 96'000, 5'000'000'000 };
    clock.outputLatencySamples = 480;
    clock.sampleRate = sampleRate;

    // 10 ms after the anchor, one 60 Hz refresh ahead: 96000 + (10 ms + 16.667 ms) * 48 kHz - 480.
    const auto heard = heardSongSample (clock, 5'000'000'000 + 10 * msNs, refreshNs);
    CHECK (heard == Catch::Approx (96'000.0 + 480.0 + 800.0 - 480.0).margin (0.01));
}

TEST_CASE ("stopped, the heard Song sample is the playhead, wherever the last anchor was", "[video][frames]")
{
    PlaybackClock clock;
    clock.playing = false;
    clock.position = 240'000;
    clock.anchor = Anchor { 10'000, 5'000'000'000 };
    clock.outputLatencySamples = 480;

    CHECK (heardSongSample (clock, 9'000'000'000, refreshNs) == Catch::Approx (240'000.0));

    // Playing before the first callback: the position too.
    clock.playing = true;
    clock.anchor.reset();
    CHECK (heardSongSample (clock, 9'000'000'000, refreshNs) == Catch::Approx (240'000.0));
}

TEST_CASE ("the frame for a heard position is the last one at or before its movie time", "[video][frames]")
{
    SongWithVideo f;
    f.addVideoTake ("v1", 48'000, timingOf30Fps (90)); // 3 s from 1 s

    // Heard 1.5 s into the Song: movie time 0.5 s, frame 15 exactly.
    auto choice = chooseFrame (f.song, f.lookup(), 48'000.0 + 24'000.0);
    CHECK (choice.videoTakeId == "v1");
    CHECK (choice.movieSeconds == Catch::Approx (0.5));
    CHECK (choice.frameIndex == std::optional<std::size_t> (15));

    // Just before frame 16's time: still frame 15.
    choice = chooseFrame (f.song, f.lookup(), 48'000.0 + 16.0 / 30.0 * 48'000.0 - 1.0);
    CHECK (choice.frameIndex == std::optional<std::size_t> (15));

    // Driven from a playing clock: an anchor at 1.2 s, 50 ms later, 10 ms output latency.
    PlaybackClock clock { .playing = true, .position = 57'600, .anchor = Anchor { 57'600, 2'000'000'000 }, .outputLatencySamples = 480, .sampleRate = sampleRate };
    const auto heard = heardSongSample (clock, 2'000'000'000 + 50 * msNs, refreshNs);
    // 57600 + 66.667 ms * 48 = 60800, less 480 = 60320: movie time 0.25667 s, frame 7 (0.2333 s).
    choice = chooseFrame (f.song, f.lookup(), heard);
    CHECK (choice.frameIndex == std::optional<std::size_t> (7));
}

TEST_CASE ("across a camera stall the frame before the gap is held", "[video][frames]")
{
    SongWithVideo f;
    // 30 fps with a 1.2 s stall after frame 14: frame 15 is at 0.5 + 1.2 = 1.7 s.
    f.addVideoTake ("v1", 0, timingOf30Fps (60, 14, 1.2));

    for (const double movieSeconds : { 14.0 / 30.0, 0.6, 1.0, 1.69 })
        CHECK (chooseFrame (f.song, f.lookup(), movieSeconds * sampleRate).frameIndex == std::optional<std::size_t> (14));
    CHECK (chooseFrame (f.song, f.lookup(), 1.7 * sampleRate + 1.0).frameIndex == std::optional<std::size_t> (15));
}

TEST_CASE ("after a seek the frame follows the new anchor, and stopped the new playhead", "[video][frames]")
{
    SongWithVideo f;
    f.addVideoTake ("v1", 0, timingOf30Fps (300)); // 10 s

    PlaybackClock clock { .playing = true, .position = 48'000, .anchor = Anchor { 48'000, 1'000'000'000 }, .outputLatencySamples = 0, .sampleRate = sampleRate };
    // Before the seek: 1 s + 16.667 ms -> frame 30.
    CHECK (chooseFrame (f.song, f.lookup(), heardSongSample (clock, 1'000'000'000, refreshNs)).frameIndex == std::optional<std::size_t> (30));

    // Seek to 7 s while playing: the next anchor is at 7 s, a few ms of host time later.
    clock.position = 7 * 48'000;
    clock.anchor = Anchor { 7 * 48'000, 1'010'000'000 };
    // 7 s + 16.667 ms -> 7.0167 s, frame 210.
    CHECK (chooseFrame (f.song, f.lookup(), heardSongSample (clock, 1'010'000'000, refreshNs)).frameIndex == std::optional<std::size_t> (210));

    // Stopped and scrubbed back to 2.5 s: frame 75, whatever the last anchor says.
    clock.playing = false;
    clock.position = 120'000;
    CHECK (chooseFrame (f.song, f.lookup(), heardSongSample (clock, 3'000'000'000, refreshNs)).frameIndex == std::optional<std::size_t> (75));
}

TEST_CASE ("where Video Takes overlap the newest Pass's is shown, and outside every Video Take none is", "[video][frames]")
{
    SongWithVideo f;
    f.addVideoTake ("old", 0, timingOf30Fps (300));           // 0 .. 10 s
    f.addVideoTake ("new", 4 * 48'000, timingOf30Fps (90));   // 4 .. 7 s
    f.addVideoTake ("late", 20 * 48'000, timingOf30Fps (30)); // 20 .. 21 s

    CHECK (chooseFrame (f.song, f.lookup(), 2.0 * sampleRate).videoTakeId == "old");
    const auto overlap = chooseFrame (f.song, f.lookup(), 5.0 * sampleRate);
    CHECK (overlap.videoTakeId == "new");
    CHECK (overlap.frameIndex == std::optional<std::size_t> (30));
    CHECK (chooseFrame (f.song, f.lookup(), 8.0 * sampleRate).videoTakeId == "old");

    const auto gap = chooseFrame (f.song, f.lookup(), 15.0 * sampleRate);
    CHECK_FALSE (gap.videoTakeId.has_value());
    CHECK_FALSE (gap.frameIndex.has_value());
    CHECK_FALSE (chooseFrame (f.song, f.lookup(), 21.0 * sampleRate + 1.0).videoTakeId.has_value());

    // The Pass order decides, not the Video Takes' order in the Song.
    std::swap (f.song.videoTakes[0], f.song.videoTakes[1]);
    CHECK (chooseFrame (f.song, f.lookup(), 5.0 * sampleRate).videoTakeId == "new");
}

TEST_CASE ("a Video Take whose movie cannot be read spans nothing and is never shown", "[video][frames]")
{
    SongWithVideo f;
    f.addVideoTake ("v1", 48'000, timingOf30Fps (90));
    f.addVideoTake ("lost", 48'000, {});
    f.timings.erase ("lost");

    const auto spans = videoTakeSpans (f.song, f.lookup());
    REQUIRE (spans.size() == 2);
    CHECK (spans[0] == VideoTakeSpan { "v1", "Pass 1", 48'000, 48'000 + 3 * 48'000, true });
    CHECK (spans[1] == VideoTakeSpan { "lost", "Pass 2", 48'000, 48'000, false });
    CHECK (chooseFrame (f.song, f.lookup(), 60'000.0).videoTakeId == "v1");
}

TEST_CASE ("a Video Take's span and movie time follow its drift ratio", "[video][frames]")
{
    SongWithVideo f;
    f.addVideoTake ("v1", 1'000, timingOf30Fps (300)); // 10 s of movie
    // The audio clock ran 100 ppm fast against the capture clock.
    f.song.videoTakes[0].songSamplesPerMovieSecond = sampleRate * 1.0001;

    const auto spans = videoTakeSpans (f.song, f.lookup());
    REQUIRE (spans.size() == 1);
    CHECK (spans[0].end == 1'000 + 480'048); // 10 s * 48004.8 samples per movie second
    CHECK (movieSecondsAt (f.song.videoTakes[0], 1'000.0 + 480'048.0) == Catch::Approx (10.0));
}
