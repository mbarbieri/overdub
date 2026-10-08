#include "capture/ClapSelftest.h"

#include "capture/MovieWriter.h"
#include "engine/EngineFixture.h"
#include "song/SongPackage.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

using overdub::capture::MovieWriter;
using overdub::capture::runClapSelftest;
using overdub::song::SongPackage;
using namespace overdub::engine::testing;

namespace
{
constexpr std::int64_t frameNs = 33'333'333;

// A grey picture with a white bar `width` pixels wide: the "hands" closing as it grows.
juce::Image frameWithBar (int width)
{
    juce::Image image (juce::Image::ARGB, 160, 120, false);
    image.clear (image.getBounds(), juce::Colours::grey);
    if (width > 0)
        image.clear ({ 0, 40, width, 40 }, juce::Colours::white);
    return image;
}

// A Song with one recorded Pass: a Take with a clap heard at Song sample 48240 (1.005 s) and a
// Video Take from Song sample 0 at 48000 samples per movie second, whose bar grows over frames
// 28-31 and stops: the hands meet between frames 30 and 31.
juce::File packageWithClapPass (const TempPackage& folder)
{
    auto song = overdub::song::makeEmptySong (testSampleRate);
    song.passes.push_back ({ "pass-1", 0, "2026-10-08T10:00:00Z" });
    overdub::song::Take take;
    take.id = "take-1";
    take.trackId = "track-1";
    take.passId = "pass-1";
    take.file = "audio/take-1.wav";
    take.lengthSamples = 3 * testSampleRate;
    song.takes.push_back (take);
    song.videoTakes.push_back ({ .id = "video-1", .passId = "pass-1", .file = "video/video-1.mov", .startSample = 0, .songSamplesPerMovieSecond = testSampleRate, .cameraId = "camera-facetime", .calibrationOffsetSamples = 0 });

    const auto location = folder.root.getChildFile ("Clap.overdub");
    auto created = SongPackage::create (location, song);
    REQUIRE (created.has_value());
    created->reset();

    writeFloatWav (location.getChildFile (take.file), { impulse (take.lengthSamples, 48'240) });

    auto writer = MovieWriter::create (location.getChildFile ("video/video-1.mov"), 160, 120);
    REQUIRE (writer.has_value());
    for (int frame = 0; frame < 60; ++frame)
        REQUIRE ((*writer)->appendImage (frameWithBar (std::clamp (frame - 27, 0, 4) * 40), frame * frameNs));
    REQUIRE ((*writer)->finish().has_value());
    return location;
}
} // namespace

TEST_CASE ("the clap selftest reports each clap's offset between the Take and the Video Take", "[capture][video][clap][selftest]")
{
    TempPackage folder;
    const auto report = runClapSelftest (packageWithClapPass (folder));

    CHECK (report["selftest"] == "clap");
    CHECK (report["pass"] == true);
    CHECK (report["pass_id"] == "pass-1");
    CHECK (report["take_id"] == "take-1");
    CHECK (report["video_take_id"] == "video-1");
    CHECK (report["camera_id"] == "camera-facetime");
    REQUIRE (report["claps"].size() == 1);
    CHECK (report["claps"][0]["song_sample"] == 48'240);
    CHECK (report["claps"][0]["offset_ms"].get<double>() == Catch::Approx ((30.5 * frameNs / 1.0e9 - 1.005) * 1000.0).margin (0.01));
}

TEST_CASE ("the clap selftest fails explicitly without a Video Take to measure", "[capture][video][clap][selftest]")
{
    TempPackage folder;
    const auto location = folder.root.getChildFile ("Empty.overdub");
    REQUIRE (SongPackage::create (location, overdub::song::makeEmptySong (testSampleRate)).has_value());

    const auto report = runClapSelftest (location);
    CHECK (report["pass"] == false);
    CHECK (report["error"] == "capture.clap_no_video_take");

    const auto missing = runClapSelftest (folder.root.getChildFile ("Missing.overdub"));
    CHECK (missing["pass"] == false);
    CHECK (missing["error"] == "song.read_failed");
    CHECK (missing["error_fields"].contains ("path"));
}
