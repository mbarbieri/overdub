#include "export/MovieExport.h"

#include "capture/MovieWriter.h"
#include "core/Log.h"
#include "engine/EngineFixture.h"
#include "export/Mp4Decode.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <algorithm>
#include <cmath>
#include <optional>
#include <utility>
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
        videoTake = song::VideoTake { "video-1", "pass-1", "video/video-1.mov", videoStart, driftRatio, "camera-test", 0, "Speakers" };
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
    REQUIRE (destination.getParentDirectory().createDirectory().wasOk());

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
    REQUIRE (destination.getParentDirectory().createDirectory().wasOk());

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

TEST_CASE ("a Song with a Video Take but no Clips exports an MP4 of the Video Take over silence", "[export][mp4][no_clips]")
{
    FlashFixture f;
    f.mix.tracks.clear(); // the Video Take stays; nothing is heard
    MixRender render { f.mix, f.package.root, 256, {} };
    REQUIRE (render.totalSamples() == 0);

    const auto [length, range] = GENERATE (std::pair { VideoLength::trimToVideoTake, SampleRange { videoStart, videoEnd() } },
                                           std::pair { VideoLength::fullSong, SampleRange { 0, videoEnd() } });
    CAPTURE (overdub::exporting::videoLengthName (length));
    const auto destination = f.package.root.getChildFile ("exports/Silent.mp4");
    REQUIRE (destination.getParentDirectory().createDirectory().wasOk());

    core::ScopedLogCapture capture;
    std::vector<double> progress;
    const auto exported = overdub::exporting::exportMovie (render, f.request (length, destination), f.source(), [&progress] (double fraction) { progress.push_back (fraction); });
    REQUIRE (exported.has_value());
    REQUIRE (destination.existsAsFile());
    // No partial movie and no mix file left beside it.
    CHECK (destination.getParentDirectory().findChildFiles (juce::File::findFilesAndDirectories, false, "*", juce::File::FollowSymlinks::no).size() == 1);
    CHECK (exported->samples == range.length());
    CHECK (std::abs (movieDurationSeconds (destination) - (double) range.length() / testSampleRate) < 1.0 / fps);

    // Every flash is in the movie, where its click would be.
    const auto flashes = flashTimes (decodeVideoFrames (destination));
    REQUIRE (flashes.size() == std::size (flashFrames));
    for (std::size_t i = 0; i < flashes.size(); ++i)
    {
        const auto expected = (double) (clickSampleOf (flashFrames[i]) - range.start) / testSampleRate;
        CAPTURE (i, flashes[i], expected);
        CHECK (std::abs (flashes[i] - expected) <= halfFrame);
    }

    // The audio track is silence as long as the movie.
    const auto audio = decodeAudio (destination, testSampleRate);
    REQUIRE (audio.getNumChannels() > 0);
    CHECK (std::abs ((double) audio.getNumSamples() - (double) range.length()) < (double) testSampleRate / fps);
    CHECK (audio.getMagnitude (0, audio.getNumSamples()) <= 1.0e-6f); // -120 dBFS, the null-test bound (ADR 0005)

    REQUIRE (! progress.empty());
    CHECK (std::ranges::is_sorted (progress));
    CHECK (std::abs (progress.back() - 1.0) < 1.0e-9);
    const auto started = eventsNamed (capture, "export.started");
    REQUIRE (started.size() == 1);
    CHECK (started[0]["start_sample"] == range.start);
    CHECK (started[0]["end_sample"] == range.end);
    CHECK (eventsNamed (capture, "export.finished").size() == 1);
    destination.deleteFile();
}

TEST_CASE ("a stopped MP4 export of a Song with no Clips leaves no file", "[export][mp4][no_clips]")
{
    FlashFixture f;
    f.mix.tracks.clear();
    MixRender render { f.mix, f.package.root, 256, {} };
    const auto destination = f.package.root.getChildFile ("exports/Stopped.mp4");
    REQUIRE (destination.getParentDirectory().createDirectory().wasOk());

    // Stopped while the silent mix is written, before the encode starts.
    int asked = 0;
    const auto exported = overdub::exporting::exportMovie (render, f.request (VideoLength::fullSong, destination), f.source(), {}, [&asked] { return ++asked > 3; });
    REQUIRE_FALSE (exported.has_value());
    CHECK (exported.error().code == "export.cancelled");
    CHECK (exported.error().fields.at ("samples_written") == 3 * 256);
    CHECK (destination.getParentDirectory().findChildFiles (juce::File::findFilesAndDirectories, false, "*", juce::File::FollowSymlinks::no).isEmpty());
}

TEST_CASE ("an MP4 of a Video Take that ends before the Song starts is refused, not blamed on the Clips", "[export][mp4][outside_song]")
{
    FlashFixture f;
    // The 10 s Video Take ends 1 s before Song sample 0. Trimmed, the Song keeps its Clips; the full
    // song is empty only without Clips.
    f.videoTake.startSample = -11 * testSampleRate;
    const auto length = GENERATE (VideoLength::trimToVideoTake, VideoLength::fullSong);
    CAPTURE (overdub::exporting::videoLengthName (length));
    if (length == VideoLength::fullSong)
        f.mix.tracks.clear();
    MixRender render { f.mix, f.package.root, 256, {} };
    REQUIRE (render.totalSamples() == (length == VideoLength::fullSong ? 0 : mixLength));
    const auto destination = f.package.root.getChildFile ("exports/Before.mp4");

    core::ScopedLogCapture capture;
    const auto exported = overdub::exporting::exportMovie (render, f.request (length, destination), f.source());
    REQUIRE_FALSE (exported.has_value());
    CHECK (exported.error().code == "export.video_take_outside_song");
    CHECK (exported.error().fields.at ("video_take_id") == "video-1");
    CHECK (exported.error().fields.at ("video_take_end_sample") < 0);
    CHECK_FALSE (destination.exists());
    CHECK_FALSE (destination.getParentDirectory().exists()); // nothing was started
    CHECK (eventsNamed (capture, "export.started").empty());
}

TEST_CASE ("a Loop's MP4 is exactly its range: the Video Take inside it, every flash on its click, black where the Video Take does not cover it", "[export][mp4][loop]")
{
    FlashFixture f;
    MixRender render { f.mix, f.package.root, 256, {} };
    const auto destination = f.package.root.getChildFile ("Loop.mp4");
    // Starts before the Video Take and ends inside it: flashes 15 and 100 are in it.
    const auto loop = SampleRange { testSampleRate / 2, 5 * testSampleRate };
    auto request = f.request (VideoLength::trimToVideoTake, destination);
    request.loop = overdub::exporting::ExportLoop { "loop-1", loop };

    core::ScopedLogCapture capture;
    const auto exported = overdub::exporting::exportMovie (render, request, f.source());
    REQUIRE (exported.has_value());
    CHECK (exported->samples == loop.length());
    CHECK (std::abs (movieDurationSeconds (destination) - (double) loop.length() / testSampleRate) < 1.0 / fps);

    const auto frames = decodeVideoFrames (destination);
    const auto flashes = flashTimes (frames);
    REQUIRE (flashes.size() == 2);
    const auto audio = decodeAudio (destination, testSampleRate);
    for (std::size_t i = 0; i < flashes.size(); ++i)
    {
        const auto expected = (double) (clickSampleOf (flashFrames[i]) - loop.start) / testSampleRate;
        CAPTURE (i, flashes[i], expected);
        CHECK (std::abs (flashes[i] - expected) <= halfFrame);
        CHECK (std::abs (flashes[i] - clickTimeNear (audio, expected)) <= halfFrame);
    }
    const auto videoStartSeconds = (double) (videoStart - loop.start) / testSampleRate;
    int blackBefore = 0;
    for (const auto& frame : frames)
        if (frame.seconds < videoStartSeconds - 1.0 / fps)
        {
            CHECK (frame.meanLuma < 8.0);
            ++blackBefore;
        }
    CHECK (blackBefore >= fps / 2 - 2);

    const auto started = eventsNamed (capture, "export.started");
    REQUIRE (started.size() == 1);
    CHECK (started[0]["length"] == "loop");
    CHECK (started[0]["loop_id"] == "loop-1");
    CHECK (started[0]["start_sample"] == loop.start);
    CHECK (started[0]["end_sample"] == loop.end);
}

TEST_CASE ("a Loop's MP4 before the Video Take starts or after it ends is black for the Loop's whole length", "[export][mp4][loop]")
{
    FlashFixture f;
    MixRender render { f.mix, f.package.root, 256, {} };
    const auto destination = f.package.root.getChildFile ("Outside.mp4");
    const auto loop = GENERATE (SampleRange { 0, videoStart - testSampleRate / 4 }, SampleRange { videoEnd() + testSampleRate / 4, mixLength });
    CAPTURE (loop.start, loop.end);
    auto request = f.request (VideoLength::fullSong, destination);
    request.loop = overdub::exporting::ExportLoop { "loop-1", loop };

    const auto exported = overdub::exporting::exportMovie (render, request, f.source());
    REQUIRE (exported.has_value());
    CHECK (exported->samples == loop.length());
    CHECK (std::abs (movieDurationSeconds (destination) - (double) loop.length() / testSampleRate) < 1.0 / fps);
    const auto frames = decodeVideoFrames (destination);
    REQUIRE (! frames.empty());
    for (const auto& frame : frames)
        CHECK (frame.meanLuma < 8.0);
}
