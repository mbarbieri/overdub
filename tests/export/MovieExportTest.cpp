#include "export/MovieExport.h"

#include "capture/MovieWriter.h"
#include "core/Log.h"
#include "engine/EngineFixture.h"
#include "export/Mp4Decode.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <optional>
#include <vector>

// Export to MP4 (issue #38) as a round trip (ADR 0005): a synthetic 10 s movie with flash frames
// and a mix with a click at the Song sample each flash is placed on go through the real trimmed
// and full-song exports; decoding the MP4 finds each flash within ±½ frame of its click, and the
// offset moves by at most 1 ms from the first marker to the last. The Video Take's drift ratio is
// 500 ppm off nominal, so an export that ignored it would move the last marker 5 ms.
using overdub::exporting::ExportFormat;
using overdub::exporting::ExportRequest;
using overdub::exporting::ExportResult;
using overdub::exporting::MixRender;
using overdub::exporting::MovieSource;
using overdub::exporting::SampleRange;
using overdub::exporting::VideoLength;
using namespace overdub::engine::testing;
using namespace overdub::exporting::testing;
namespace core = overdub::core;
namespace song = overdub::song;

namespace
{
constexpr int fps = 30;
constexpr int movieFrames = 10 * fps;        // a 10 s movie
constexpr song::SampleCount mixLength = 12 * testSampleRate;
constexpr song::SampleCount videoStart = testSampleRate + 777; // ~1 s in, off any frame grid
constexpr double driftRatio = testSampleRate * (1.0 + 500.0e-6);
constexpr int flashFrames[] { 15, 100, 200, 285 };
constexpr double halfFrame = 0.5 / fps;

std::int64_t framePtsNs (int frame)
{
    return 7'000'000'000'000 + (std::int64_t) std::llround (frame * 1.0e9 / fps);
}

double movieSecondsOf (int frame)
{
    return (double) (framePtsNs (frame) - framePtsNs (0)) / 1.0e9;
}

// Where a flash frame starts on the Song clock: the click that goes with it.
song::SampleCount clickSampleOf (int frame)
{
    return videoStart + (song::SampleCount) std::llround (movieSecondsOf (frame) * driftRatio);
}

// Dark grey frames, white at each flash frame, written through the real movie writer.
void writeFlashMovie (const juce::File& file)
{
    REQUIRE (file.getParentDirectory().createDirectory().wasOk());
    auto created = overdub::capture::MovieWriter::create (file, 160, 120);
    REQUIRE (created.has_value());
    for (int frame = 0; frame < movieFrames; ++frame)
    {
        const auto flash = std::ranges::contains (flashFrames, frame);
        juce::Image image (juce::Image::ARGB, 160, 120, false);
        image.clear (image.getBounds(), flash ? juce::Colours::white : juce::Colour::greyLevel (0.25f));
        REQUIRE ((*created)->appendImage (image, framePtsNs (frame)));
    }
    REQUIRE ((*created)->finish().has_value());
}

struct FlashFixture
{
    TempPackage package;
    song::Song mix = song::makeEmptySong (testSampleRate);
    song::VideoTake videoTake;
    juce::File movie;

    FlashFixture()
    {
        std::vector<float> clicks ((std::size_t) mixLength, 0.0f);
        for (const auto frame : flashFrames)
            clicks[(std::size_t) clickSampleOf (frame)] = 0.8f;
        const auto take = addTake (mix, package, "take-clicks", clicks);
        mix.tracks = { trackWith ("clicks", { clipOf ("clip-clicks", take, 0, 0, mixLength) }) };

        movie = package.root.getChildFile ("video/video-1.mov");
        writeFlashMovie (movie);
        videoTake = song::VideoTake { "video-1", "pass-1", "video/video-1.mov", videoStart, driftRatio, "camera-test", 0 };
        mix.passes.push_back (song::Pass { "pass-1", videoStart, "2026-10-08T12:00:00Z" });
        mix.videoTakes.push_back (videoTake);
    }

    ExportRequest request (VideoLength length, const juce::File& destination) const
    {
        return ExportRequest { ExportFormat::mp4, destination, videoTake.id, length };
    }

    MovieSource source() const { return MovieSource { movie, videoTake }; }
};

// The start of each flash in the decoded movie: the first bright frame after a dark one.
std::vector<double> flashTimes (const std::vector<DecodedVideoFrame>& frames)
{
    std::vector<double> times;
    for (std::size_t i = 0; i < frames.size(); ++i)
        if (frames[i].meanLuma > 160.0 && (i == 0 || frames[i - 1].meanLuma < 160.0))
            times.push_back (frames[i].seconds);
    return times;
}

// The loudest sample within 50 ms of `expected`, in seconds.
double clickTimeNear (const juce::AudioBuffer<float>& audio, double expectedSeconds)
{
    const auto centre = (int) std::llround (expectedSeconds * testSampleRate);
    const auto from = std::max (0, centre - testSampleRate / 20);
    const auto to = std::min (audio.getNumSamples(), centre + testSampleRate / 20);
    REQUIRE (from < to);
    const auto* samples = audio.getReadPointer (0);
    const auto loudest = std::max_element (samples + from, samples + to, [] (float a, float b) { return std::abs (a) < std::abs (b); });
    REQUIRE (std::abs (*loudest) > 0.1f);
    return (double) (loudest - samples) / testSampleRate;
}

// Each flash's offset from its click (video − audio), in seconds, in the exported file.
std::vector<double> markerOffsets (const juce::File& file, song::SampleCount exportStart)
{
    const auto flashes = flashTimes (decodeVideoFrames (file));
    REQUIRE (flashes.size() == std::size (flashFrames));
    const auto audio = decodeAudio (file, testSampleRate);
    REQUIRE (audio.getNumSamples() > 0);
    std::vector<double> offsets;
    for (std::size_t i = 0; i < flashes.size(); ++i)
    {
        const auto expectedAudio = (double) (clickSampleOf (flashFrames[i]) - exportStart) / testSampleRate;
        offsets.push_back (flashes[i] - clickTimeNear (audio, expectedAudio));
    }
    return offsets;
}

void checkMarkersInSync (const std::vector<double>& offsets)
{
    for (const auto offset : offsets)
    {
        CAPTURE (offset);
        CHECK (std::abs (offset) <= halfFrame); // ±½ frame per marker (ADR 0005)
    }
    CAPTURE (offsets.front(), offsets.back());
    CHECK (std::abs (offsets.back() - offsets.front()) <= 0.001); // end − start ≤ 1 ms
}

std::vector<nlohmann::json> eventsNamed (const core::ScopedLogCapture& capture, const std::string& name)
{
    std::vector<nlohmann::json> found;
    for (const auto& event : capture.events())
        if (event.value ("event", "") == name)
            found.push_back (event);
    return found;
}

// From the Video Take's start to the end of its last frame (taken as long as the mean one).
constexpr song::SampleCount videoEnd()
{
    return videoStart + (song::SampleCount) (10.0 * driftRatio + 0.5);
}
} // namespace

TEST_CASE ("an MP4 trimmed to the Video Take keeps every flash on its click", "[export][mp4][round_trip]")
{
    FlashFixture f;
    MixRender render { f.mix, f.package.root, 256, {} };
    const auto destination = f.package.root.getChildFile ("exports/Trimmed.mp4");

    core::ScopedLogCapture capture;
    std::vector<double> progress;
    const auto exported = overdub::exporting::exportMovie (render, f.request (VideoLength::trimToVideoTake, destination), f.source(), [&progress] (double fraction) { progress.push_back (fraction); });
    REQUIRE (exported.has_value());
    CHECK (exported->file == destination);
    REQUIRE (destination.existsAsFile());
    // No partial movie and no mix file left beside it.
    CHECK (destination.getParentDirectory().findChildFiles (juce::File::findFilesAndDirectories, false, "*", juce::File::FollowSymlinks::no).size() == 1);

    const auto range = SampleRange { videoStart, videoEnd() };
    CHECK (exported->samples == range.length());
    CHECK (std::abs (movieDurationSeconds (destination) - (double) range.length() / testSampleRate) < 1.0 / fps);
    checkMarkersInSync (markerOffsets (destination, range.start));

    REQUIRE (! progress.empty());
    CHECK (std::ranges::is_sorted (progress));
    CHECK (std::abs (progress.back() - 1.0) < 1.0e-9);

    const auto started = eventsNamed (capture, "export.started");
    REQUIRE (started.size() == 1);
    CHECK (started[0]["format"] == "mp4");
    CHECK (started[0]["video_take_id"] == "video-1");
    CHECK (started[0]["length"] == "trim");
    CHECK (started[0]["start_sample"] == range.start);
    CHECK (started[0]["end_sample"] == range.end);
    const auto finished = eventsNamed (capture, "export.finished");
    REQUIRE (finished.size() == 1);
    CHECK (finished[0]["format"] == "mp4");
}

TEST_CASE ("a full-song MP4 keeps every flash on its click and is black outside the Video Take", "[export][mp4][round_trip]")
{
    FlashFixture f;
    MixRender render { f.mix, f.package.root, 256, {} };
    const auto destination = f.package.root.getChildFile ("Full.mp4");

    const auto exported = overdub::exporting::exportMovie (render, f.request (VideoLength::fullSong, destination), f.source());
    REQUIRE (exported.has_value());
    REQUIRE (destination.existsAsFile());
    CHECK (exported->samples == mixLength);
    CHECK (std::abs (movieDurationSeconds (destination) - (double) mixLength / testSampleRate) < 1.0 / fps);
    checkMarkersInSync (markerOffsets (destination, 0));

    // Black before the Video Take starts and after it ends (one frame of margin for the re-timing).
    const auto frames = decodeVideoFrames (destination);
    REQUIRE (! frames.empty());
    const auto startSeconds = (double) videoStart / testSampleRate;
    const auto endSeconds = (double) videoEnd() / testSampleRate;
    int blackBefore = 0, blackAfter = 0;
    for (const auto& frame : frames)
    {
        if (frame.seconds < startSeconds - 1.0 / fps)
        {
            CHECK (frame.meanLuma < 8.0);
            ++blackBefore;
        }
        else if (frame.seconds > endSeconds + 1.0 / fps)
        {
            CHECK (frame.meanLuma < 8.0);
            ++blackAfter;
        }
        else if (frame.seconds > startSeconds + 1.0 / fps && frame.seconds < endSeconds - 1.0 / fps)
            CHECK (frame.meanLuma > 40.0); // the movie's grey or a flash
    }
    CHECK (blackBefore >= fps - 2);
    CHECK (blackAfter >= fps - 2);
}

TEST_CASE ("a stopped MP4 export leaves no file", "[export][mp4]")
{
    FlashFixture f;
    MixRender render { f.mix, f.package.root, 256, {} };
    const auto destination = f.package.root.getChildFile ("exports/Stopped.mp4");

    int asked = 0;
    const auto exported = overdub::exporting::exportMovie (render, f.request (VideoLength::trimToVideoTake, destination), f.source(), {}, [&asked] { return ++asked > 3; });
    REQUIRE_FALSE (exported.has_value());
    CHECK (exported.error().code == "export.cancelled");
    CHECK (destination.getParentDirectory().findChildFiles (juce::File::findFilesAndDirectories, false, "*", juce::File::FollowSymlinks::no).isEmpty());
}

TEST_CASE ("a Video Take whose movie cannot be read is an explicit error and leaves no file", "[export][mp4]")
{
    FlashFixture f;
    MixRender render { f.mix, f.package.root, 256, {} };
    const auto destination = f.package.root.getChildFile ("exports/Missing.mp4");
    auto source = f.source();
    source.movie = f.package.root.getChildFile ("video/missing.mov");

    const auto exported = overdub::exporting::exportMovie (render, f.request (VideoLength::fullSong, destination), source);
    REQUIRE_FALSE (exported.has_value());
    CHECK (exported.error().code == "export.video_take_unreadable");
    CHECK (exported.error().fields.at ("video_take_id") == "video-1");
    CHECK_FALSE (destination.exists());
    CHECK_FALSE (destination.getParentDirectory().exists()); // nothing was started
}
