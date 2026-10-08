#include "video/MovieFile.h"

#include "capture/MovieWriter.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <vector>

// The playback side of a movie (issue #36) on synthetic movies written through the real
// AVFoundation writer: frame times across a camera stall, and the decoder reading forward and
// seeking back and ahead.
using namespace overdub::video;
using overdub::capture::MovieWriter;

namespace
{
struct TempFolder
{
    juce::File root = juce::File::getSpecialLocation (juce::File::tempDirectory)
                          .getChildFile ("overdub-video-test-" + juce::Uuid().toString());

    TempFolder() { root.createDirectory(); }
    ~TempFolder() { root.deleteRecursively(); }
};

constexpr std::int64_t frameNs = 33'333'333;
constexpr std::int64_t stallNs = 1'200'000'000;
constexpr std::int64_t firstPtsNs = 7'000'000'000'000;
constexpr int frameCount = 90;

// 30 fps from a large host-clock PTS, with a 1.2 s camera stall after frame 14.
std::int64_t ptsOf (int frame)
{
    return firstPtsNs + frame * frameNs + (frame >= 15 ? stallNs : 0);
}

// The movie stores PTS at its own timescale: a time asked for lies 1 ms into its frame.
constexpr double insideFrame = 0.001;

double secondsOf (int frame)
{
    return (double) (ptsOf (frame) - firstPtsNs) / 1.0e9;
}

juce::File writeStallMovie (const TempFolder& folder)
{
    const auto file = folder.root.getChildFile ("take.mov");
    auto created = MovieWriter::create (file, 160, 120);
    REQUIRE (created.has_value());
    for (int frame = 0; frame < frameCount; ++frame)
    {
        juce::Image image (juce::Image::ARGB, 160, 120, false);
        image.clear (image.getBounds(), juce::Colour::greyLevel ((float) (frame % 10) / 10.0f));
        REQUIRE ((*created)->appendImage (image, ptsOf (frame)));
    }
    REQUIRE ((*created)->finish().has_value());
    return file;
}
} // namespace

TEST_CASE ("a movie's frame times start at its first frame and keep a camera stall", "[video][movie]")
{
    TempFolder folder;
    const auto movie = writeStallMovie (folder);

    const auto timing = readMovieTiming (movie);
    REQUIRE (timing.has_value());
    REQUIRE (timing->frameSeconds.size() == (std::size_t) frameCount);
    for (int frame = 0; frame < frameCount; ++frame)
        CHECK (timing->frameSeconds[(std::size_t) frame] == Catch::Approx (secondsOf (frame)).margin (1.0e-6));
    // To the end of the last frame, the last frame as long as the mean one.
    const auto span = secondsOf (frameCount - 1);
    CHECK (timing->durationSeconds == Catch::Approx (span + span / (frameCount - 1)).margin (1.0e-6));
}

TEST_CASE ("the decoder reads frames forward, and restarts for a step back or a jump ahead", "[video][movie]")
{
    TempFolder folder;
    const auto movie = writeStallMovie (folder);
    auto timing = readMovieTiming (movie);
    REQUIRE (timing.has_value());
    auto opened = FrameDecoder::open (movie, *timing);
    REQUIRE (opened.has_value());
    auto& decoder = **opened;

    // Forward through the stall: no restart, each the asked frame.
    for (const int frame : { 0, 1, 2, 10, 14, 15, 16 })
    {
        const auto decoded = decoder.decode (secondsOf (frame) + insideFrame);
        REQUIRE (decoded.has_value());
        CHECK (decoded->seconds == Catch::Approx (secondsOf (frame)).margin (1.0e-6));
        CHECK (decoded->sampleBuffer != nullptr);
        CHECK_FALSE (decoded->restarted);
    }

    // A time inside the stall shows the frame before the gap: a step back from frame 16. Asked
    // again, the same frame without reading.
    auto held = decoder.decode (secondsOf (14) + 0.6);
    REQUIRE (held.has_value());
    CHECK (held->seconds == Catch::Approx (secondsOf (14)).margin (1.0e-6));
    CHECK (held->restarted);
    held = decoder.decode (secondsOf (14) + 0.7);
    REQUIRE (held.has_value());
    CHECK (held->seconds == Catch::Approx (secondsOf (14)).margin (1.0e-6));
    CHECK_FALSE (held->restarted);

    // A seek back, then a jump ahead of more than a second.
    for (const int frame : { 3, 80 })
    {
        const auto decoded = decoder.decode (secondsOf (frame) + insideFrame);
        REQUIRE (decoded.has_value());
        CHECK (decoded->seconds == Catch::Approx (secondsOf (frame)).margin (1.0e-6));
        CHECK (decoded->restarted);
    }
    const auto next = decoder.decode (secondsOf (81) + insideFrame);
    REQUIRE (next.has_value());
    CHECK (next->seconds == Catch::Approx (secondsOf (81)).margin (1.0e-6));
    CHECK_FALSE (next->restarted);
}

TEST_CASE ("a movie that is not a movie is an explicit error for its timing and its decoder", "[video][movie]")
{
    TempFolder folder;
    const auto notAMovie = folder.root.getChildFile ("fake.mov");
    REQUIRE (notAMovie.replaceWithText ("fake movie"));

    const auto timing = readMovieTiming (notAMovie);
    REQUIRE_FALSE (timing.has_value());
    CHECK (timing.error().code == "video.movie_unreadable");
    const auto decoder = FrameDecoder::open (folder.root.getChildFile ("missing.mov"), MovieTiming { { 0.0 }, 1.0 / 30.0 });
    REQUIRE_FALSE (decoder.has_value());
    CHECK (decoder.error().code == "video.movie_unreadable");
}
