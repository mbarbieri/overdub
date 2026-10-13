#pragma once

#include "capture/MovieWriter.h"
#include "song/Song.h"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <string>

// Synthetic Video Take movies for tests (ADR 0005): grey frames written through the real
// AVFoundation writer, and a Song's Pass and Video Take that play them. No camera, no golden files.
namespace overdub::video::testing
{
// Writes `frames` 30 fps frames (160x120), from a large host-clock PTS, into `file`.
inline void writeSyntheticMovie (const juce::File& file, int frames)
{
    REQUIRE (file.getParentDirectory().createDirectory().wasOk());
    auto created = capture::MovieWriter::create (file, 160, 120);
    REQUIRE (created.has_value());
    for (int frame = 0; frame < frames; ++frame)
    {
        juce::Image image (juce::Image::ARGB, 160, 120, false);
        image.clear (image.getBounds(), juce::Colour::greyLevel ((float) (frame % 10) / 10.0f));
        REQUIRE ((*created)->appendImage (image, 9'000'000'000'000 + (std::int64_t) frame * 33'333'333));
    }
    REQUIRE ((*created)->finish().has_value());
}

// Adds a Pass at `startSample` and its Video Take, at the nominal rate, to `song`, and writes its
// movie of `frames` frames into the package at `packageRoot`. Returns the Video Take's id.
inline song::Id addSyntheticVideoTake (song::Song& song, const juce::File& packageRoot, song::SampleCount startSample, int frames)
{
    const auto passId = song::makeId();
    const auto videoTakeId = song::makeId();
    song.passes.push_back (song::Pass { passId, startSample, "2026-10-08T12:00:00Z" });
    const auto file = "video/" + videoTakeId + ".mov";
    song.videoTakes.push_back (song::VideoTake { .id = videoTakeId,
                                                 .passId = passId,
                                                 .file = file,
                                                 .startSample = startSample,
                                                 .songSamplesPerMovieSecond = (double) song.sampleRate,
                                                 .cameraId = "camera-facetime",
                                                 .calibrationOffsetSamples = 0,
                                                 .outputDevice = "MacBook Pro Speakers" });
    writeSyntheticMovie (packageRoot.getChildFile (file), frames);
    return videoTakeId;
}
} // namespace overdub::video::testing
