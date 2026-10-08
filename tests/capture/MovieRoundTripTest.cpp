#include "capture/MovieFrames.h"
#include "capture/MovieWriter.h"

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstdint>
#include <vector>

using overdub::capture::MovieWriter;
using overdub::capture::readMovieFrames;

namespace
{
struct TempFolder
{
    juce::File root = juce::File::getSpecialLocation (juce::File::tempDirectory)
                          .getChildFile ("overdub-movie-test-" + juce::Uuid().toString());

    TempFolder() { root.createDirectory(); }
    ~TempFolder() { root.deleteRecursively(); }
};

juce::Image frameOf (juce::Colour colour)
{
    juce::Image image (juce::Image::ARGB, 160, 120, false);
    image.clear (image.getBounds(), colour);
    return image;
}

constexpr std::int64_t frameNs = 33'333'333;
constexpr std::int64_t stallNs = 1'200'000'000;
constexpr std::int64_t firstPtsNs = 5'000'000'000'000;

// Frame i's PTS: 30 fps from a large host-clock PTS, with a 1.2 s camera stall after frame 14.
std::int64_t ptsOf (int frame)
{
    return firstPtsNs + frame * frameNs + (frame >= 15 ? stallNs : 0);
}
} // namespace

TEST_CASE ("a written movie starts at its first frame and keeps every frame's timestamp across a stall", "[capture][video][movie]")
{
    TempFolder folder;
    // The recorder writes <id>.mov.tmp and renames it: the writer must accept that name.
    const auto file = folder.root.getChildFile ("take.mov.tmp");
    auto created = MovieWriter::create (file, 160, 120);
    REQUIRE (created.has_value());
    auto& writer = **created;

    // Grey frames with white flashes at frames 10 and 20.
    for (int frame = 0; frame < 30; ++frame)
        REQUIRE (writer.appendImage (frameOf (frame == 10 || frame == 20 ? juce::Colours::white : juce::Colours::grey), ptsOf (frame)));
    const auto stats = writer.finish();
    REQUIRE (stats.has_value());
    CHECK (stats->frames == 30);
    CHECK (stats->refusedFrames == 0);
    CHECK (stats->firstPtsNs == firstPtsNs);
    CHECK (stats->longestGapNs == frameNs + stallNs);

    // Renamed into place as the recorder does: AVFoundation reads a movie by its extension.
    const auto movie = folder.root.getChildFile ("take.mov");
    REQUIRE (file.moveFileTo (movie));
    const auto frames = readMovieFrames (movie);
    REQUIRE (frames.has_value());
    REQUIRE (frames->size() == 30);
    for (int frame = 0; frame < 30; ++frame)
        CHECK (std::abs ((*frames)[(std::size_t) frame].seconds - (double) (ptsOf (frame) - firstPtsNs) / 1.0e9) < 1.0e-6);

    // Motion is the change into and out of each flash; nothing else moves.
    for (int frame = 1; frame < 30; ++frame)
    {
        const auto motion = (*frames)[(std::size_t) frame].motion;
        const auto flashEdge = frame == 10 || frame == 11 || frame == 20 || frame == 21;
        if (flashEdge)
            CHECK (motion > 50.0);
        else
            CHECK (motion < 5.0);
    }
}

TEST_CASE ("a movie with no frames is an explicit error and leaves no file", "[capture][video][movie]")
{
    TempFolder folder;
    const auto file = folder.root.getChildFile ("empty.mov.tmp");
    auto created = MovieWriter::create (file, 160, 120);
    REQUIRE (created.has_value());

    const auto finished = (*created)->finish();
    REQUIRE_FALSE (finished.has_value());
    CHECK (finished.error().code == "capture.movie_no_frames");
    CHECK_FALSE (file.exists());
}

TEST_CASE ("reading a missing movie is an explicit error", "[capture][video][movie]")
{
    TempFolder folder;
    const auto frames = readMovieFrames (folder.root.getChildFile ("missing.mov"));
    REQUIRE_FALSE (frames.has_value());
    CHECK (frames.error().code == "capture.movie_unreadable");
}
