#include "song/TransposeRender.h"

#include "core/Log.h"
#include "song/AtomicWrite.h"
#include "song/AudioFileDecode.h"
#include "song/BackingTrackImport.h"
#include "song/Transpose.h"

#include "AudioMeasurements.h"
#include "FixtureSongBuilder.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <thread>
#include <vector>

using Catch::Matchers::WithinAbs;
using namespace overdub::song::testing;

namespace
{
constexpr int sampleRate = 48000;
constexpr double sineFrequency = 440.0;

// `seconds` of a stereo sine.
juce::AudioBuffer<float> sineBuffer (double seconds, double frequency)
{
    juce::AudioBuffer<float> sine (2, (int) (seconds * sampleRate));
    for (int i = 0; i < sine.getNumSamples(); ++i)
        for (int channel = 0; channel < 2; ++channel)
            sine.setSample (channel, i, 0.25f * (float) std::sin (2.0 * juce::MathConstants<double>::pi * frequency * i / sampleRate));
    return sine;
}

// A temp folder holding a 2 s stereo 440 Hz sine as a derived Backing Track WAV.
struct RenderFixture
{
    juce::File folder = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("overdub-transpose-" + juce::Uuid().toString());
    juce::File source = folder.getChildFile ("backing.wav");
    juce::AudioBuffer<float> sine = sineBuffer (2.0, sineFrequency);

    RenderFixture()
    {
        REQUIRE (folder.createDirectory().wasOk());
        REQUIRE (overdub::song::writeFloatWavAtomically (source, sine, sampleRate, {}).has_value());
    }
    ~RenderFixture() { folder.deleteRecursively(); }
};

// A temp folder, deleted with everything in it.
struct ScratchFolder
{
    juce::File folder = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("overdub-transpose-" + juce::Uuid().toString());

    ScratchFolder() { REQUIRE (folder.createDirectory().wasOk()); }
    ~ScratchFolder() { folder.deleteRecursively(); }
};
} // namespace

TEST_CASE ("a sine rendered at a Transpose comes out at the computed frequency and the same length", "[song][transpose][render]")
{
    // Whole semitones (issue #209), and semitones with cents in one pass (issue #232).
    const auto transpose = GENERATE (overdub::song::Transpose { .semitones = 12 },
                                     overdub::song::Transpose { .semitones = -1 },
                                     overdub::song::Transpose { .semitones = 0, .cents = 30 },
                                     overdub::song::Transpose { .semitones = -12, .cents = -50 });
    INFO (transpose.semitones << " semitones " << transpose.cents << " cents");
    RenderFixture fixture;
    const auto destination = fixture.folder.getChildFile ("backing.transpose.wav");

    REQUIRE (overdub::song::renderTransposed (fixture.source, destination, sampleRate, transpose, {}).has_value());

    const auto rendered = overdub::song::readWavFile (destination);
    REQUIRE (rendered.has_value());
    CHECK (rendered->getNumChannels() == 2);
    CHECK (rendered->getNumSamples() == fixture.sine.getNumSamples());
    // 880 Hz, 415.30 Hz, 447.69 Hz, 213.74 Hz
    const auto expected = sineFrequency * std::pow (2.0, (transpose.semitones + transpose.cents / 100.0) / 12.0);
    // Measured away from both ends, where the stretcher has the whole spectral window. ADR 0005:
    // within 5 cents.
    for (int channel = 0; channel < 2; ++channel)
    {
        const auto measured = measuredFrequency (rendered->getReadPointer (channel), sampleRate / 4, sampleRate * 7 / 4, sampleRate);
        CHECK_THAT (1200.0 * std::log2 (measured / expected), WithinAbs (0.0, 5.0));
    }
}

TEST_CASE ("a sine is rendered within 5 cents of its shifted pitch across the bass and mid range, at 44.1 and 48 kHz", "[song][transpose][render]")
{
    struct Case
    {
        int rate;
        double frequency;
        int semitones;
    };
    // The cases presetDefault's window put furthest off (up to 17 cents).
    const auto c = GENERATE (Case { 48000, 110.0, 3 }, Case { 48000, 110.0, -2 }, Case { 44100, 220.0, -2 }, Case { 48000, 330.0, -1 });
    INFO (c.frequency << " Hz at " << c.rate << " Hz, " << c.semitones << " semitones");
    const ScratchFolder scratch;
    const auto& folder = scratch.folder;
    juce::AudioBuffer<float> sine { 1, 2 * c.rate };
    for (int i = 0; i < sine.getNumSamples(); ++i)
        sine.setSample (0, i, 0.25f * (float) std::sin (2.0 * juce::MathConstants<double>::pi * c.frequency * i / c.rate));
    const auto source = folder.getChildFile ("backing.wav");
    const auto destination = folder.getChildFile ("backing.transpose.wav");
    REQUIRE (overdub::song::writeFloatWavAtomically (source, sine, c.rate, {}).has_value());

    REQUIRE (overdub::song::renderTransposed (source, destination, c.rate, { .semitones = c.semitones }, {}).has_value());

    const auto rendered = overdub::song::readWavFile (destination);
    REQUIRE (rendered.has_value());
    const auto expected = c.frequency * std::pow (2.0, c.semitones / 12.0);
    const auto measured = measuredFrequency (rendered->getReadPointer (0), c.rate / 4, c.rate * 7 / 4, c.rate);
    CHECK_THAT (1200.0 * std::log2 (measured / expected), WithinAbs (0.0, 5.0));
}

TEST_CASE ("a render that cannot read its source writes nothing and names the source", "[song][transpose][render]")
{
    RenderFixture fixture;
    const auto destination = fixture.folder.getChildFile ("backing.transpose.wav");

    const auto failed = overdub::song::renderTransposed (fixture.folder.getChildFile ("missing.wav"), destination, sampleRate, { .semitones = 3 }, {});

    REQUIRE_FALSE (failed.has_value());
    CHECK (failed.error().code == "song.transpose_render_failed");
    CHECK (failed.error().fields.at ("step") == "read");
    CHECK (failed.error().fields.at ("path") == fixture.folder.getChildFile ("missing.wav").getFullPathName().toStdString());
    CHECK_FALSE (destination.exists());
}

TEST_CASE ("a render asked to stop writes nothing", "[song][transpose][render]")
{
    RenderFixture fixture;
    const auto destination = fixture.folder.getChildFile ("backing.transpose.wav");
    std::stop_source stop;
    stop.request_stop();

    const auto cancelled = overdub::song::renderTransposed (fixture.source, destination, sampleRate, { .semitones = 3 }, stop.get_token());

    REQUIRE_FALSE (cancelled.has_value());
    CHECK (cancelled.error().code == "song.transpose_render_cancelled");
    CHECK_FALSE (destination.exists());
    CHECK_FALSE (overdub::song::tempFileFor (destination).exists());
}

namespace
{
// Runs a TransposeRun to the end, as the window's timer would.
std::optional<overdub::core::Expected<void>> runToEnd (overdub::song::TransposeRun& run, std::optional<overdub::song::TransposeRun::Clock::time_point> endedAt = std::nullopt)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds (30);
    while (std::chrono::steady_clock::now() < deadline)
    {
        if (auto ended = run.advance (endedAt.value_or (overdub::song::TransposeRun::Clock::now())))
            return ended;
        std::this_thread::sleep_for (std::chrono::milliseconds (5));
    }
    FAIL ("the Transpose run did not end");
    return std::nullopt;
}

std::vector<nlohmann::json> eventsNamed (const overdub::core::ScopedLogCapture& capture, const std::string& name)
{
    std::vector<nlohmann::json> found;
    for (const auto& event : capture.events())
        if (event.at ("event") == name)
            found.push_back (event);
    return found;
}
} // namespace

TEST_CASE ("a Song at Transpose 0 needs no render, and at +2 one per Backing Track without one", "[song][transpose][render]")
{
    auto fixture = FixtureSongBuilder().backingTrack ("Band").backingTrack ("Click").build();
    const auto root = fixture->package->location();
    auto song = fixture->song();

    CHECK (overdub::song::missingTransposeRenders (root, song).empty());

    song.transpose = { .semitones = 2 };
    const auto missing = overdub::song::missingTransposeRenders (root, song);
    REQUIRE (missing.size() == 2);
    CHECK (missing[0].backingTrackId == song.backingTracks[0].id);
    CHECK (missing[0].source == overdub::song::derivedFileFor (root, song.backingTracks[0]));
    CHECK (missing[0].destination == overdub::song::transposedFileFor (root, song.backingTracks[0], { .semitones = 2 }));

    REQUIRE (missing[0].destination.replaceWithText ("rendered"));
    const auto stillMissing = overdub::song::missingTransposeRenders (root, song);
    REQUIRE (stillMissing.size() == 1);
    CHECK (stillMissing[0].backingTrackId == song.backingTracks[1].id);
}

TEST_CASE ("only the renders of the kept Transpose stay in the package", "[song][transpose][render]")
{
    auto fixture = FixtureSongBuilder().backingTrack ("Band").build();
    const auto root = fixture->package->location();
    const auto& backingTrack = fixture->song().backingTracks[0];
    using overdub::song::Transpose;
    for (const auto transpose : { Transpose { .semitones = -1 }, Transpose { .semitones = 2 }, Transpose { .semitones = 12 }, Transpose { .semitones = 2, .cents = 20 }, Transpose { .semitones = 0, .cents = 2 } })
        REQUIRE (overdub::song::transposedFileFor (root, backingTrack, transpose).replaceWithText ("rendered"));

    CHECK (overdub::song::deleteTransposeRendersExcept (root, { .semitones = 2 }) == 4);

    CHECK (overdub::song::transposedFileFor (root, backingTrack, { .semitones = 2 }).existsAsFile());
    CHECK_FALSE (overdub::song::transposedFileFor (root, backingTrack, { .semitones = -1 }).exists());
    CHECK_FALSE (overdub::song::transposedFileFor (root, backingTrack, { .semitones = 12 }).exists());
    CHECK_FALSE (overdub::song::transposedFileFor (root, backingTrack, { .semitones = 2, .cents = 20 }).exists());
    CHECK_FALSE (overdub::song::transposedFileFor (root, backingTrack, { .semitones = 0, .cents = 2 }).exists());
    CHECK (overdub::song::derivedFileFor (root, backingTrack).existsAsFile());
    CHECK (overdub::song::originalFileFor (root, backingTrack).existsAsFile());
}

TEST_CASE ("a Transpose run renders every missing render on a worker and logs its start and end", "[song][transpose][render]")
{
    auto fixture = FixtureSongBuilder().backingTrack ("Band", 1.0).backingTrack ("Click", 1.0).build();
    const auto root = fixture->package->location();
    auto song = fixture->song();
    song.transpose = { .semitones = -1, .cents = 25 };
    overdub::core::ScopedLogCapture capture;

    auto run = overdub::song::TransposeRun::start (song.sampleRate, song.transpose, overdub::song::missingTransposeRenders (root, song), overdub::song::TransposeRun::Clock::now());
    CHECK (run->transpose() == overdub::song::Transpose { .semitones = -1, .cents = 25 });
    const auto ended = runToEnd (*run);

    REQUIRE (ended.has_value());
    CHECK (ended->has_value());
    CHECK (overdub::song::missingTransposeRenders (root, song).empty());
    const auto started = eventsNamed (capture, "song.transpose_started");
    REQUIRE (started.size() == 1);
    CHECK (started[0].at ("semitones") == -1);
    CHECK (started[0].at ("cents") == 25);
    CHECK (started[0].at ("renders") == 2);
    CHECK (started[0].at ("max_renders_at_once") == overdub::song::transposeRendersAtOnce ((int) std::thread::hardware_concurrency()));
    const auto rendered = eventsNamed (capture, "song.transpose_rendered");
    REQUIRE (rendered.size() == 1);
    CHECK (rendered[0].at ("renders") == 2);
    CHECK (rendered[0].contains ("duration_ms"));
}

TEST_CASE ("a Transpose run that cannot render names the Backing Track", "[song][transpose][render]")
{
    auto fixture = FixtureSongBuilder().backingTrack ("Band", 1.0).build();
    const auto root = fixture->package->location();
    auto song = fixture->song();
    song.transpose = { .semitones = 4 };
    auto jobs = overdub::song::missingTransposeRenders (root, song);
    REQUIRE (overdub::song::derivedFileFor (root, song.backingTracks[0]).deleteFile());

    auto run = overdub::song::TransposeRun::start (song.sampleRate, song.transpose, jobs, overdub::song::TransposeRun::Clock::now());
    const auto ended = runToEnd (*run);

    REQUIRE (ended.has_value());
    REQUIRE_FALSE (ended->has_value());
    CHECK (ended->error().code == "song.transpose_render_failed");
    CHECK (ended->error().fields.at ("backing_track_id") == song.backingTracks[0].id);
    CHECK_FALSE (jobs[0].destination.exists());
}

namespace
{
using overdub::song::TransposeRenderJob;
using overdub::song::TransposeRun;

// Stand-ins for renders on the run's workers, which stay running until the test lets them go or the
// run stops them, so how many run at once is observed rather than timed. Catch's macros stay on the
// test's thread.
class HeldRenders
{
public:
    void arrive()
    {
        const std::scoped_lock guard (lock);
        ++started;
        ++running;
        mostAtOnce = std::max (mostAtOnce, running);
        changed.notify_all();
    }

    // Returns true when let go, false when the run stopped it.
    bool hold (std::stop_token stop)
    {
        arrive();
        std::unique_lock guard (lock);
        const auto wasLetGo = changed.wait (guard, stop, [this] { return allowed > 0; });
        if (wasLetGo)
            --allowed;
        else
            ++stopsHeard;
        --running;
        changed.notify_all();
        return wasLetGo;
    }

    void letGo (int count)
    {
        const std::scoped_lock guard (lock);
        allowed += count;
        changed.notify_all();
    }

    bool waitForRunning (int count)
    {
        return waitUntil ([this, count] { return running == count; });
    }
    bool waitForStarted (int count)
    {
        return waitUntil ([this, count] { return started >= count; });
    }

    int startedCount() { return read (started); }
    int mostRunningAtOnce() { return read (mostAtOnce); }
    int stopsHeardCount() { return read (stopsHeard); }

private:
    template <typename Condition>
    bool waitUntil (Condition condition)
    {
        std::unique_lock guard (lock);
        return changed.wait_for (guard, std::chrono::seconds (30), condition);
    }

    int read (const int& value)
    {
        const std::scoped_lock guard (lock);
        return value;
    }

    std::mutex lock;
    std::condition_variable_any changed;
    int started = 0;
    int running = 0;
    int mostAtOnce = 0;
    int allowed = 0;
    int stopsHeard = 0;
};

constexpr overdub::song::Transpose runTranspose { .semitones = 3, .cents = 20 };

// Jobs for renders that touch no file.
std::vector<TransposeRenderJob> jobsWithoutFiles (int count)
{
    std::vector<TransposeRenderJob> jobs;
    for (int i = 0; i < count; ++i)
        jobs.push_back ({ .backingTrackId = "backing-" + std::to_string (i), .source = {}, .destination = {} });
    return jobs;
}

// A run of `jobCount` file-less jobs whose renders are all held.
std::unique_ptr<TransposeRun> startHeld (HeldRenders& held, int jobCount, TransposeRun::Clock::time_point now, int cap)
{
    return TransposeRun::start (runTranspose, jobsWithoutFiles (jobCount), now, cap, [&held] (const TransposeRenderJob&, std::stop_token stop) -> overdub::core::Expected<void> {
        if (held.hold (stop))
            return {};
        return std::unexpected (overdub::core::Error { "song.transpose_render_cancelled", "stopped", {} });
    });
}

// `seconds` of a stereo sine as a derived Backing Track WAV.
void writeSine (const juce::File& file, double seconds, double frequency)
{
    REQUIRE (overdub::song::writeFloatWavAtomically (file, sineBuffer (seconds, frequency), sampleRate, {}).has_value());
}

// Jobs that render the same real source into `folder`.
std::vector<TransposeRenderJob> jobsOf (const juce::File& source, const juce::File& folder, int count)
{
    auto jobs = jobsWithoutFiles (count);
    for (auto& job : jobs)
    {
        job.source = source;
        job.destination = folder.getChildFile (job.backingTrackId + ".transpose.wav");
    }
    return jobs;
}

int tempFilesIn (const juce::File& folder)
{
    return folder.findChildFiles (juce::File::findFiles, false, "*.tmp").size();
}
} // namespace

TEST_CASE ("a machine renders at most 4 Backing Tracks at once and keeps a core free", "[song][transpose][render]")
{
    using overdub::song::transposeRendersAtOnce;
    CHECK (transposeRendersAtOnce (0) == 1);
    CHECK (transposeRendersAtOnce (1) == 1);
    CHECK (transposeRendersAtOnce (2) == 1);
    CHECK (transposeRendersAtOnce (3) == 2);
    CHECK (transposeRendersAtOnce (5) == 4);
    CHECK (transposeRendersAtOnce (16) == 4);
}

TEST_CASE ("a Transpose run renders as many jobs at once as its cap allows, and no more", "[song][transpose][render]")
{
    const auto cap = GENERATE (1, 2, 4, 8);
    constexpr int jobCount = 6;
    HeldRenders held;

    auto run = startHeld (held, jobCount, TransposeRun::Clock::now(), cap);

    // Letting go of one render at a time: its worker takes the next job, so the cap stays full until the jobs run out.
    for (int finished = 0; finished < jobCount; ++finished)
    {
        REQUIRE (held.waitForRunning (std::min (cap, jobCount - finished)));
        held.letGo (1);
    }
    const auto ended = runToEnd (*run);

    REQUIRE (ended.has_value());
    CHECK (ended->has_value());
    CHECK (held.startedCount() == jobCount);
    CHECK (held.mostRunningAtOnce() == std::min (cap, jobCount));
}

TEST_CASE ("a parallel Transpose run logs its start and an end whose duration covers the whole run", "[song][transpose][render]")
{
    const auto startedAt = TransposeRun::Clock::time_point { std::chrono::hours (1) };
    overdub::core::ScopedLogCapture capture;
    HeldRenders held;

    auto run = startHeld (held, 3, startedAt, 2);
    held.letGo (3);
    const auto ended = runToEnd (*run, startedAt + std::chrono::seconds (5));

    REQUIRE (ended.has_value());
    CHECK (ended->has_value());
    const auto started = eventsNamed (capture, "song.transpose_started");
    REQUIRE (started.size() == 1);
    CHECK (started[0].at ("semitones") == 3);
    CHECK (started[0].at ("cents") == 20);
    CHECK (started[0].at ("renders") == 3);
    CHECK (started[0].at ("max_renders_at_once") == 2);
    const auto rendered = eventsNamed (capture, "song.transpose_rendered");
    REQUIRE (rendered.size() == 1);
    CHECK (rendered[0].at ("semitones") == 3);
    CHECK (rendered[0].at ("cents") == 20);
    CHECK (rendered[0].at ("renders") == 3);
    CHECK (rendered[0].at ("duration_ms") == 5000);
}

TEST_CASE ("cancelling a parallel Transpose run stops every running render, starts no other and reports the renders done", "[song][transpose][render]")
{
    const auto startedAt = TransposeRun::Clock::time_point { std::chrono::hours (1) };
    overdub::core::ScopedLogCapture capture;
    HeldRenders held;

    auto run = startHeld (held, 5, startedAt, 2);
    REQUIRE (held.waitForRunning (2));
    held.letGo (2);
    // A worker takes its next job only after counting the render it finished.
    REQUIRE (held.waitForStarted (4));
    REQUIRE (held.waitForRunning (2));

    run->cancel (startedAt + std::chrono::seconds (2));

    CHECK (held.stopsHeardCount() == 2);
    CHECK (held.startedCount() == 4);
    const auto cancelled = eventsNamed (capture, "song.transpose_cancelled");
    REQUIRE (cancelled.size() == 1);
    CHECK (cancelled[0].at ("semitones") == 3);
    CHECK (cancelled[0].at ("cents") == 20);
    CHECK (cancelled[0].at ("renders_done") == 2);
    CHECK (cancelled[0].at ("renders") == 5);
    CHECK (cancelled[0].at ("duration_ms") == 2000);

    CHECK_FALSE (run->advance (startedAt + std::chrono::seconds (3)).has_value());
    run->cancel (startedAt + std::chrono::seconds (3));
    CHECK (eventsNamed (capture, "song.transpose_cancelled").size() == 1);
    CHECK (eventsNamed (capture, "song.transpose_rendered").empty());
}

TEST_CASE ("cancelling real renders in parallel leaves no partial or temp file", "[song][transpose][render]")
{
    ScratchFolder scratch;
    const auto source = scratch.folder.getChildFile ("backing.wav");
    writeSine (source, 30.0, sineFrequency);
    const auto jobs = jobsOf (source, scratch.folder, 4);
    overdub::core::ScopedLogCapture capture;
    HeldRenders held;

    auto run = TransposeRun::start (runTranspose, jobs, TransposeRun::Clock::now(), 2, [&held] (const TransposeRenderJob& job, std::stop_token stop) {
        held.arrive();
        return overdub::song::renderTransposed (job.source, job.destination, sampleRate, runTranspose, stop);
    });
    REQUIRE (held.waitForStarted (2));
    run->cancel (TransposeRun::Clock::now());

    const auto cancelled = eventsNamed (capture, "song.transpose_cancelled");
    REQUIRE (cancelled.size() == 1);
    // A render past its last block when the stop came is whole; every other leaves nothing.
    int written = 0;
    for (const auto& job : jobs)
        if (job.destination.existsAsFile())
        {
            ++written;
            const auto rendered = overdub::song::readWavFile (job.destination);
            REQUIRE (rendered.has_value());
            CHECK (rendered->getNumSamples() == 30 * sampleRate);
        }
    CHECK (cancelled[0].at ("renders_done") == written);
    CHECK (tempFilesIn (scratch.folder) == 0);
}

TEST_CASE ("the first failed render ends a parallel run with its Error and stops the others; the renders written stay", "[song][transpose][render]")
{
    ScratchFolder scratch;
    const auto source = scratch.folder.getChildFile ("backing.wav");
    writeSine (source, 1.0, sineFrequency);
    auto jobs = jobsOf (source, scratch.folder, 4);
    jobs[1].source = scratch.folder.getChildFile ("missing.wav");
    overdub::core::ScopedLogCapture capture;
    HeldRenders held;

    // Jobs are taken in order. The first is written; the third runs by then and is still running
    // when the second fails, and the fourth must never start.
    auto run = TransposeRun::start (runTranspose, jobs, TransposeRun::Clock::now(), 2, [&] (const TransposeRenderJob& job, std::stop_token stop) -> overdub::core::Expected<void> {
        if (job.backingTrackId == jobs[1].backingTrackId)
            held.waitForStarted (1);
        else if (job.backingTrackId != jobs[0].backingTrackId)
            held.hold (stop);
        return overdub::song::renderTransposed (job.source, job.destination, sampleRate, runTranspose, stop);
    });
    const auto ended = runToEnd (*run);

    REQUIRE (ended.has_value());
    REQUIRE_FALSE (ended->has_value());
    CHECK (ended->error().code == "song.transpose_render_failed");
    CHECK (ended->error().fields.at ("backing_track_id") == jobs[1].backingTrackId);
    CHECK (ended->error().fields.at ("step") == "read");
    CHECK (held.startedCount() == 1);
    CHECK (held.stopsHeardCount() == 1);
    CHECK (jobs[0].destination.existsAsFile());
    CHECK_FALSE (jobs[1].destination.exists());
    CHECK_FALSE (jobs[2].destination.exists());
    CHECK_FALSE (jobs[3].destination.exists());
    CHECK (tempFilesIn (scratch.folder) == 0);
    CHECK (eventsNamed (capture, "song.transpose_rendered").empty());
    run->cancel (TransposeRun::Clock::now());
    CHECK (eventsNamed (capture, "song.transpose_cancelled").empty());
}

TEST_CASE ("a render made beside others equals the same render made alone, sample for sample", "[song][transpose][render]")
{
    ScratchFolder scratch;
    constexpr int jobCount = 4;
    auto jobs = jobsWithoutFiles (jobCount);
    std::vector<juce::File> aloneRenders;
    for (int i = 0; i < jobCount; ++i)
    {
        jobs[(size_t) i].source = scratch.folder.getChildFile (jobs[(size_t) i].backingTrackId + ".wav");
        jobs[(size_t) i].destination = scratch.folder.getChildFile (jobs[(size_t) i].backingTrackId + ".beside.wav");
        writeSine (jobs[(size_t) i].source, 2.0, 220.0 * (i + 1));
        aloneRenders.push_back (scratch.folder.getChildFile (jobs[(size_t) i].backingTrackId + ".alone.wav"));
        REQUIRE (overdub::song::renderTransposed (jobs[(size_t) i].source, aloneRenders.back(), sampleRate, runTranspose, {}).has_value());
    }
    HeldRenders held;
    std::atomic<int> startedTogether { 0 };

    // No render starts until all have arrived, so they overlap whatever the machine does.
    auto run = TransposeRun::start (runTranspose, jobs, TransposeRun::Clock::now(), jobCount, [&] (const TransposeRenderJob& job, std::stop_token stop) {
        held.arrive();
        if (held.waitForStarted (jobCount))
            ++startedTogether;
        return overdub::song::renderTransposed (job.source, job.destination, sampleRate, runTranspose, stop);
    });
    const auto ended = runToEnd (*run);

    REQUIRE (ended.has_value());
    REQUIRE (ended->has_value());
    CHECK (startedTogether.load() == jobCount);
    for (int i = 0; i < jobCount; ++i)
    {
        const auto alone = overdub::song::readWavFile (aloneRenders[(size_t) i]);
        const auto beside = overdub::song::readWavFile (jobs[(size_t) i].destination);
        REQUIRE (alone.has_value());
        REQUIRE (beside.has_value());
        REQUIRE (alone->getNumChannels() == beside->getNumChannels());
        REQUIRE (alone->getNumSamples() == beside->getNumSamples());
        CHECK (alone->getRMSLevel (0, 0, alone->getNumSamples()) > 0.1f);
        int differing = 0;
        for (int channel = 0; channel < alone->getNumChannels(); ++channel)
            for (int sample = 0; sample < alone->getNumSamples(); ++sample)
                if (alone->getSample (channel, sample) != beside->getSample (channel, sample))
                    ++differing;
        CHECK (differing == 0);
    }
}
