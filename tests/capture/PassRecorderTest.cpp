#include "capture/PassRecorder.h"
#include "capture/LoopbackMeasurement.h"
#include "song/AudioFileDecode.h"
#include "song/Peaks.h"

#include "engine/EngineFixture.h"
#include "engine/TestProcessors.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <memory>
#include <string>
#include <vector>

using overdub::capture::PassRecorder;
using overdub::capture::PassRecorderOptions;
using overdub::capture::RecordedPass;
using overdub::capture::RecordLatency;
using overdub::core::ScopedLogCapture;
using overdub::engine::Engine;
using overdub::engine::EngineConfig;
using overdub::song::SampleCount;
using overdub::song::Song;
using overdub::song::SongPackage;
using namespace overdub::engine::testing;

namespace
{
constexpr int blockSize = 256;

// A real package (SongPackage) in a temp folder, with helpers to add Tracks whose media is
// written into it. Setup is one undo step ("Set up"), so the Pass is the next one.
struct RecordingFixture
{
    TempPackage folder;
    std::unique_ptr<SongPackage> package;
    Song song = overdub::song::makeEmptySong (testSampleRate);

    RecordingFixture()
    {
        auto created = SongPackage::create (folder.root.getChildFile ("Song.overdub"), song);
        REQUIRE (created.has_value());
        package = std::move (*created);
    }

    const juce::File& root() const { return package->location(); }

    // A Track playing a Take of these channels from Song sample 0.
    void addPlayingTrack (const std::string& id, const std::vector<std::vector<float>>& channels)
    {
        overdub::song::Take take;
        take.id = id + "-take";
        take.file = "audio/" + take.id + ".wav";
        take.channels = (int) channels.size();
        take.lengthSamples = (SampleCount) channels[0].size();
        writeFloatWav (root().getChildFile (take.file), channels, testSampleRate);
        song.takes.push_back (take);
        song.tracks.push_back (trackWith (id, { clipOf (id + "-clip", take, 0, 0, take.lengthSamples) }));
    }

    void addArmedTrack (const std::string& id, std::vector<int> inputChannels)
    {
        auto track = trackWith (id, {});
        track.armed = true;
        track.inputChannels = std::move (inputChannels);
        song.tracks.push_back (track);
    }

    void commitSetup()
    {
        REQUIRE (package->edit (song, "Set up").has_value());
    }
};

// Plays from Song sample 0 and records `blocks` blocks, then stops the Pass and finishes it.
RecordedPass recordPass (RecordingFixture& fixture, Engine& engine, LoopbackDevice& device, RecordLatency latency, int blocks)
{
    auto started = PassRecorder::start (*fixture.package, engine, latency);
    REQUIRE (started.has_value());
    auto& recorder = **started;

    engine.play();
    device.run (engine, blocks);
    recorder.requestStop();
    device.run (engine, 1);
    REQUIRE (recorder.phase() == overdub::capture::PassPhase::saving);

    auto finished = recorder.poll (overdub::song::SongPackage::Clock::now());
    REQUIRE (finished.has_value()); // no camera: the first poll after the stop finishes the Pass
    if (! finished->pass.has_value())
        FAIL ("finish failed: " << finished->pass.error().code << " " << finished->pass.error().fields.dump());
    return *finished->pass;
}

const overdub::song::Take& takeOnTrack (const Song& song, const std::string& trackId)
{
    const auto take = std::ranges::find (song.takes, trackId, &overdub::song::Take::trackId);
    REQUIRE (take != song.takes.end());
    return *take;
}

const overdub::song::Track& trackNamed (const Song& song, const std::string& trackId)
{
    const auto track = std::ranges::find (song.tracks, trackId, &overdub::song::Track::id);
    REQUIRE (track != song.tracks.end());
    return *track;
}

juce::AudioBuffer<float> takeAudio (const RecordingFixture& fixture, const overdub::song::Take& take)
{
    auto audio = overdub::song::readWavFile (fixture.root().getChildFile (take.file));
    REQUIRE (audio.has_value());
    return *audio;
}

std::vector<float> channelOf (const juce::AudioBuffer<float>& audio, int channel)
{
    return { audio.getReadPointer (channel), audio.getReadPointer (channel) + audio.getNumSamples() };
}

// Where the Take's loudest sample on `channel` lands on the timeline through its Clip.
SampleCount heardPositionOfPeak (const RecordingFixture& fixture, const Song& song, const std::string& trackId, int channel)
{
    const auto& take = takeOnTrack (song, trackId);
    const auto samples = channelOf (takeAudio (fixture, take), channel);
    const auto peakIndex = (SampleCount) std::distance (samples.begin(), std::ranges::max_element (samples, {}, [] (float s) { return std::abs (s); }));

    const auto& clip = trackNamed (song, trackId).clips.back();
    REQUIRE (clip.source.id == take.id);
    REQUIRE (peakIndex >= clip.sourceOffset);
    REQUIRE (peakIndex < clip.sourceOffset + clip.length);
    CHECK (take.origin + peakIndex == clip.timelineStart + (peakIndex - clip.sourceOffset));
    return clip.timelineStart + (peakIndex - clip.sourceOffset);
}

// The Song with only the given Track, to hear it alone.
Song onlyTrack (Song song, const std::string& trackId)
{
    std::erase_if (song.tracks, [&] (const auto& track) { return track.id != trackId; });
    for (auto& track : song.tracks)
        track.armed = false;
    return song;
}

std::vector<nlohmann::json> eventsNamed (const ScopedLogCapture& capture, const std::string& name)
{
    std::vector<nlohmann::json> found;
    for (const auto& event : capture.events())
        if (event.at ("event") == name)
            found.push_back (event);
    return found;
}
} // namespace

TEST_CASE ("a mono Take places a looped-back impulse where it was played, and plays it back there", "[capture][record][placement]")
{
    // Reported input + output latency = 200 + 312 = the fake loop's 512 samples.
    RecordingFixture fixture;
    constexpr SampleCount played = 3000;
    fixture.addPlayingTrack ("track-backing", { impulse (8000, played) });
    fixture.addArmedTrack ("track-guitar", { 0 });
    fixture.commitSetup();

    Engine engine { EngineConfig { .numOutputChannels = 2, .maxBlockSize = blockSize } };
    engine.loadSong (fixture.package->song(), fixture.root());
    LoopbackDevice device { 2, blockSize, 512 };
    const auto pass = recordPass (fixture, engine, device, RecordLatency { 200, 312, 0 }, 20);

    const auto& song = fixture.package->song();
    CHECK (pass.startSample == 0);
    CHECK (heardPositionOfPeak (fixture, song, "track-guitar", 0) == played);

    // Played back alone, the recorded Track sounds the impulse at the played position.
    engine.stop();
    engine.seek (0);
    engine.loadSong (onlyTrack (song, "track-guitar"), fixture.root());
    engine.play();
    DeviceDriver playback { 2, blockSize };
    const auto rendered = playback.render (engine, 20);
    CHECK (nonZeroSamples (rendered.channels[0]) == std::vector<std::int64_t> { played });
    CHECK (nonZeroSamples (rendered.channels[1]) == std::vector<std::int64_t> { played });
}

TEST_CASE ("placement stays sample-exact under graph PDC and with a calibration offset", "[capture][record][placement][pdc]")
{
    // The real round trip is 640 samples; the device reports 500 and the calibration adds 140.
    RecordingFixture fixture;
    constexpr SampleCount played = 5000;
    fixture.addPlayingTrack ("track-backing", { impulse (10000, played) });
    fixture.addArmedTrack ("track-guitar", { 1 });
    fixture.commitSetup();

    static constexpr int pdc = 96;
    Engine engine { EngineConfig { .numOutputChannels = 2, .maxBlockSize = blockSize } };
    engine.loadSong (fixture.package->song(), fixture.root(), insertOnTrack ("track-backing", [] { return std::make_unique<DelayProcessor> (pdc); }));
    LoopbackDevice device { 2, blockSize, 640 };
    engine.seek (1000); // record from Song sample 1000
    const auto pass = recordPass (fixture, engine, device, RecordLatency { 260, 240, 140 }, 30);

    const auto& song = fixture.package->song();
    const auto& take = takeOnTrack (song, "track-guitar");
    CHECK (pass.startSample == 1000);
    CHECK (take.compensation.reportedLatencySamples == 260 + 240 + pdc);
    CHECK (take.compensation.calibrationOffsetSamples == 140);
    CHECK (take.origin == 1000 - 640);
    CHECK (heardPositionOfPeak (fixture, song, "track-guitar", 0) == played);

    // The Clip starts at the record point: what the Take captured before it is not heard.
    const auto& clip = trackNamed (song, "track-guitar").clips.back();
    CHECK (clip.timelineStart == 1000);
    CHECK (clip.sourceOffset == 640);
    CHECK (clip.length == take.lengthSamples - 640);
}

TEST_CASE ("a stereo Take places each channel's impulse where it was played", "[capture][record][placement]")
{
    RecordingFixture fixture;
    constexpr SampleCount playedLeft = 2100;
    constexpr SampleCount playedRight = 3700;
    fixture.addPlayingTrack ("track-backing", { impulse (8000, playedLeft), impulse (8000, playedRight) });
    fixture.addArmedTrack ("track-keys", { 0, 1 });
    fixture.commitSetup();

    Engine engine { EngineConfig { .numOutputChannels = 2, .maxBlockSize = blockSize } };
    engine.loadSong (fixture.package->song(), fixture.root());
    LoopbackDevice device { 2, blockSize, 700 };
    recordPass (fixture, engine, device, RecordLatency { 350, 350, 0 }, 24);

    const auto& song = fixture.package->song();
    CHECK (takeOnTrack (song, "track-keys").channels == 2);
    CHECK (heardPositionOfPeak (fixture, song, "track-keys", 0) == playedLeft);
    CHECK (heardPositionOfPeak (fixture, song, "track-keys", 1) == playedRight);
}

TEST_CASE ("two Armed Tracks record one Pass with one placed Take each", "[capture][record][placement]")
{
    RecordingFixture fixture;
    constexpr SampleCount playedLeft = 1500;
    constexpr SampleCount playedRight = 4400;
    fixture.addPlayingTrack ("track-backing", { impulse (8000, playedLeft), impulse (8000, playedRight) });
    fixture.addArmedTrack ("track-guitar", { 0 });
    fixture.addArmedTrack ("track-vocal", { 1 });
    fixture.commitSetup();

    Engine engine { EngineConfig { .numOutputChannels = 2, .maxBlockSize = blockSize } };
    engine.loadSong (fixture.package->song(), fixture.root());
    LoopbackDevice device { 2, blockSize, 300 };
    const auto pass = recordPass (fixture, engine, device, RecordLatency { 100, 200, 0 }, 24);

    const auto& song = fixture.package->song();
    REQUIRE (song.passes.size() == 1);
    CHECK (song.passes[0].id == pass.passId);
    CHECK (pass.takeIds.size() == 2);
    CHECK (takeOnTrack (song, "track-guitar").passId == pass.passId);
    CHECK (takeOnTrack (song, "track-vocal").passId == pass.passId);
    CHECK (heardPositionOfPeak (fixture, song, "track-guitar", 0) == playedLeft);
    CHECK (heardPositionOfPeak (fixture, song, "track-vocal", 0) == playedRight);
}

TEST_CASE ("a Take file nulls against the input it captured", "[capture][record][null]")
{
    RecordingFixture fixture;
    fixture.addArmedTrack ("track-stereo", { 0, 1 });
    fixture.addArmedTrack ("track-mono", { 1 });
    fixture.commitSetup();

    Engine engine { EngineConfig { .numOutputChannels = 2, .maxBlockSize = blockSize } };
    engine.loadSong (fixture.package->song(), fixture.root());
    LoopbackDevice device { 2, blockSize, blockSize };
    device.extraInput[0] = sine (40 * blockSize, 441.0, 0.7f);
    device.extraInput[1] = sine (40 * blockSize, 1234.5, 0.4f);
    device.run (engine, 3); // stopped: not part of the Pass
    const auto pass = recordPass (fixture, engine, device, RecordLatency { 128, 128, 0 }, 30);

    const auto& song = fixture.package->song();
    const auto passFrom = (std::int64_t) 3 * blockSize;
    const auto passTo = passFrom + pass.lengthSamples;
    CHECK (pass.lengthSamples == 30 * blockSize);

    const auto stereo = takeAudio (fixture, takeOnTrack (song, "track-stereo"));
    const auto mono = takeAudio (fixture, takeOnTrack (song, "track-mono"));
    REQUIRE (stereo.getNumChannels() == 2);
    REQUIRE (mono.getNumChannels() == 1);
    const auto input = [&] (int channel) { return std::vector<float> (device.inputHistory[(std::size_t) channel].begin() + passFrom, device.inputHistory[(std::size_t) channel].begin() + passTo); };
    CHECK (peakResidualDbfs (channelOf (stereo, 0), input (0)) <= -120.0);
    CHECK (peakResidualDbfs (channelOf (stereo, 1), input (1)) <= -120.0);
    CHECK (peakResidualDbfs (channelOf (mono, 0), input (1)) <= -120.0);
}

TEST_CASE ("stopping a Pass adds its Pass, Takes and Clips as exactly one undo step", "[capture][record][history]")
{
    RecordingFixture fixture;
    fixture.addPlayingTrack ("track-backing", { impulse (8000, 1000) });
    fixture.addArmedTrack ("track-guitar", { 0 });
    fixture.addArmedTrack ("track-vocal", { 1 });
    fixture.commitSetup();
    const auto before = fixture.package->song();

    Engine engine { EngineConfig { .numOutputChannels = 2, .maxBlockSize = blockSize } };
    engine.loadSong (before, fixture.root());
    LoopbackDevice device { 2, blockSize, 512 };
    recordPass (fixture, engine, device, RecordLatency { 256, 256, 0 }, 10);
    const auto after = fixture.package->song();

    CHECK (after.passes.size() == 1);
    CHECK (after.takes.size() == before.takes.size() + 2);
    CHECK (trackNamed (after, "track-guitar").clips.size() == 1);
    CHECK (trackNamed (after, "track-vocal").clips.size() == 1);
    CHECK (fixture.package->undoLabel() == "Record Pass 1");

    REQUIRE (fixture.package->undo().has_value());
    CHECK (fixture.package->song() == before);
    CHECK (fixture.package->undoLabel() == "Set up");

    REQUIRE (fixture.package->redo().has_value());
    CHECK (fixture.package->song() == after);

    // The Take files and their peaks are in the package, under the Take ids.
    for (const auto& take : after.takes)
    {
        if (take.passId.empty())
            continue;
        CHECK (fixture.root().getChildFile (take.file).existsAsFile());
        CHECK_FALSE (overdub::song::tempFileFor (fixture.root().getChildFile (take.file)).exists());
        CHECK (overdub::song::peaksFileFor (fixture.root(), take.id).existsAsFile());
        const auto peaks = fixture.package->peaks ({ overdub::song::ClipSourceKind::take, take.id });
        REQUIRE (peaks.has_value());
        CHECK (*peaks == overdub::song::computePeaks (takeAudio (fixture, take)));
    }
}

TEST_CASE ("a Pass keeps one anchor per callback for the Video Take", "[capture][record][anchor]")
{
    RecordingFixture fixture;
    fixture.addArmedTrack ("track-guitar", { 0 });
    fixture.commitSetup();

    Engine engine { EngineConfig { .numOutputChannels = 2, .maxBlockSize = blockSize } };
    engine.loadSong (fixture.package->song(), fixture.root());
    LoopbackDevice device { 2, blockSize, blockSize };
    device.hostTimeNs = 9'000'000'000;
    const auto pass = recordPass (fixture, engine, device, RecordLatency {}, 12);

    REQUIRE (pass.anchors.size() == 12);
    for (std::size_t block = 0; block < pass.anchors.size(); ++block)
    {
        CHECK (pass.anchors[block].songSample == (SampleCount) block * blockSize);
        CHECK (pass.anchors[block].hostTimeNs == 9'000'000'000ULL + block * device.blockDurationNs());
    }
}

TEST_CASE ("a Pass logs capture.pass_started and capture.pass_stopped", "[capture][record][log]")
{
    ScopedLogCapture capture;
    RecordingFixture fixture;
    fixture.addArmedTrack ("track-guitar", { 0 });
    fixture.addArmedTrack ("track-keys", { 0, 1 });
    fixture.commitSetup();

    Engine engine { EngineConfig { .numOutputChannels = 2, .maxBlockSize = blockSize } };
    engine.loadSong (fixture.package->song(), fixture.root());
    LoopbackDevice device { 2, blockSize, blockSize };
    engine.seek (4096);
    const auto pass = recordPass (fixture, engine, device, RecordLatency { 10, 20, 3 }, 8);

    const auto started = eventsNamed (capture, "capture.pass_started");
    REQUIRE (started.size() == 1);
    CHECK (started[0]["thread"] == "io");
    CHECK (started[0]["pass_id"] == pass.passId);
    CHECK (started[0]["track_ids"] == nlohmann::json { "track-guitar", "track-keys" });
    CHECK (started[0]["input_channels"] == nlohmann::json { { 0 }, { 0, 1 } });
    CHECK (started[0]["camera"] == false);
    CHECK (started[0]["start_sample"] == 4096);
    CHECK (started[0]["calibration_offset_samples"] == 3);

    const auto stopped = eventsNamed (capture, "capture.pass_stopped");
    REQUIRE (stopped.size() == 1);
    CHECK (stopped[0]["pass_id"] == pass.passId);
    CHECK (stopped[0]["outcome"] == "recorded");
    CHECK (stopped[0]["take_ids"] == pass.takeIds);
    CHECK (stopped[0]["start_sample"] == 4096);
    CHECK (stopped[0]["length_samples"] == 8 * blockSize);
    CHECK (stopped[0]["stop_reason"] == "requested");
    CHECK (stopped[0]["overrun_frames"] == 0);
    CHECK (stopped[0]["dropped_anchors"] == 0);
}

TEST_CASE ("a Pass with no Armed Track input cannot start", "[capture][record]")
{
    ScopedLogCapture capture;
    RecordingFixture fixture;
    fixture.addArmedTrack ("track-no-input", {});
    fixture.commitSetup();

    Engine engine { EngineConfig { .numOutputChannels = 2, .maxBlockSize = blockSize } };
    const auto started = PassRecorder::start (*fixture.package, engine, RecordLatency {});

    REQUIRE_FALSE (started.has_value());
    CHECK (started.error().code == "capture.nothing_armed");
    const auto skipped = eventsNamed (capture, "capture.armed_track_skipped");
    REQUIRE (skipped.size() == 1);
    CHECK (skipped[0]["track_id"] == "track-no-input");
    CHECK (skipped[0]["reason"] == "no_input_channels");
}

TEST_CASE ("a Pass stopped before any block played captures nothing and changes nothing", "[capture][record]")
{
    RecordingFixture fixture;
    fixture.addArmedTrack ("track-guitar", { 0 });
    fixture.commitSetup();
    const auto before = fixture.package->song();

    Engine engine { EngineConfig { .numOutputChannels = 2, .maxBlockSize = blockSize } };
    auto started = PassRecorder::start (*fixture.package, engine, RecordLatency {});
    REQUIRE (started.has_value());
    (*started)->requestStop();

    const auto finished = (*started)->finish().pass;
    REQUIRE_FALSE (finished.has_value());
    CHECK (finished.error().code == "capture.pass_empty");
    CHECK (fixture.package->song() == before);
    CHECK (fixture.package->undoLabel() == "Set up");
    CHECK (fixture.root().getChildFile ("audio").findChildFiles (juce::File::findFiles, false, "*.tmp").isEmpty());
}

TEST_CASE ("a Pass that overran its FIFO keeps its full length on the timeline", "[capture][record]")
{
    RecordingFixture fixture;
    constexpr SampleCount played = 6000;
    fixture.addPlayingTrack ("track-backing", { impulse (12000, played) });
    fixture.addArmedTrack ("track-guitar", { 0 });
    fixture.commitSetup();

    Engine engine { EngineConfig { .numOutputChannels = 2, .maxBlockSize = blockSize } };
    engine.loadSong (fixture.package->song(), fixture.root());
    LoopbackDevice device { 2, blockSize, 512 };

    // A FIFO of 4 blocks that the io thread does not drain while the blocks run.
    PassRecorderOptions options;
    options.fifoSeconds = 4.0 * blockSize / testSampleRate;
    options.ioInterval = std::chrono::milliseconds (60'000);
    auto started = PassRecorder::start (*fixture.package, engine, RecordLatency { 256, 256, 0 }, options);
    REQUIRE (started.has_value());
    engine.play();
    device.run (engine, 40);
    (*started)->requestStop();
    device.run (engine, 1);
    const auto pass = (*started)->finish().pass;
    REQUIRE (pass.has_value());

    CHECK (pass->overrunFrames > 0);
    CHECK (pass->lengthSamples == 40 * blockSize);
    CHECK (takeOnTrack (fixture.package->song(), "track-guitar").lengthSamples == 40 * blockSize);
}

TEST_CASE ("a Pass without a camera goes from waiting to capturing to saving, and the first poll after the stop finishes it", "[capture][record]")
{
    RecordingFixture fixture;
    fixture.addArmedTrack ("track-guitar", { 0 });
    fixture.commitSetup();
    Engine engine { EngineConfig { .numOutputChannels = 2, .maxBlockSize = blockSize } };
    engine.loadSong (fixture.package->song(), fixture.root());
    LoopbackDevice device { 2, blockSize, 512 };
    auto started = PassRecorder::start (*fixture.package, engine, RecordLatency {});
    REQUIRE (started.has_value());
    auto& recorder = **started;
    CHECK (recorder.phase() == overdub::capture::PassPhase::waiting);
    CHECK_FALSE (recorder.poll (overdub::song::SongPackage::Clock::now()).has_value());

    engine.play();
    device.run (engine, 10);
    CHECK (recorder.phase() == overdub::capture::PassPhase::capturing);
    CHECK_FALSE (recorder.poll (overdub::song::SongPackage::Clock::now()).has_value());

    recorder.requestStop();
    device.run (engine, 1);
    CHECK (recorder.phase() == overdub::capture::PassPhase::saving);
    const auto finished = recorder.poll (overdub::song::SongPackage::Clock::now());

    REQUIRE (finished.has_value());
    CHECK (finished->stopReason == overdub::engine::RecordStopReason::requested);
    REQUIRE (finished->pass.has_value());
    CHECK (finished->pass->lengthSamples == 10 * blockSize);
    CHECK (recorder.phase() == overdub::capture::PassPhase::finished);
}
