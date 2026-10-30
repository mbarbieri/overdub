#include "ui/OpenSong.h"

#include "core/Log.h"
#include "engine/EngineFixture.h"
#include "song/LoopEdits.h"
#include "song/PracticeSpeed.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <chrono>
#include <cmath>
#include <memory>
#include <thread>
#include <vector>

using Catch::Matchers::WithinAbs;
using overdub::engine::Engine;
using overdub::engine::EngineConfig;
using overdub::ui::AudioBinding;
using overdub::ui::OpenSong;
using namespace overdub::engine::testing;

// While a Loop loops in Transcribe mode, a Practice Speed change renders the Loop's window first and
// plays it, then the whole Song in the background (issue #231, ADR 0009 "The active Loop renders
// first").
namespace
{
constexpr std::int64_t earlyClickAt = 48000;  // Song time 1 s: outside the Loop's window
constexpr std::int64_t loopClickAt = 216000;  // Song time 4.5 s: inside the Loop
constexpr std::int64_t loopStart = 192000;    // 4 s
constexpr std::int64_t loopEnd = 240000;      // 5 s
constexpr auto margin = (std::int64_t) (overdub::song::loopRenderMarginSeconds * testSampleRate);
constexpr std::int64_t windowStart = loopStart - margin;
constexpr std::int64_t windowEnd = loopEnd + margin;

// An OpenSong bound to an engine, over a Song with one 6 s Backing Track (impulses at 1 s and 4.5 s)
// and a Loop "loop" over 4..5 s drilled at `loopSpeedPercent`, in Transcribe mode.
struct LoopRenderFixture
{
    TempPackage folder;
    juce::File root = folder.root.getChildFile ("Song.overdub");
    Engine engine { EngineConfig { .numOutputChannels = 2, .maxBlockSize = 256 } };
    std::unique_ptr<OpenSong> openSong;
    DeviceDriver device { 2, 256 };

    explicit LoopRenderFixture (int loopSpeedPercent = 100)
    {
        auto clicks = std::vector<float> (6 * 48000, 0.0f);
        clicks[(std::size_t) earlyClickAt] = 1.0f;
        clicks[(std::size_t) loopClickAt] = 1.0f;
        const auto wav = folder.root.getChildFile ("Clicks.wav");
        writeFloatWav (wav, { clicks });
        auto created = overdub::song::SongPackage::create (root, overdub::song::makeEmptySong (testSampleRate));
        REQUIRE (created.has_value());
        REQUIRE ((*created)->importBackingTrack (wav).has_value());
        auto withLoop = overdub::song::addLoop ((*created)->song(), loopStart, loopEnd, "loop", loopSpeedPercent);
        REQUIRE (withLoop.has_value());
        REQUIRE ((*created)->edit (*withLoop, overdub::song::addLoopLabel).has_value());
        openSong = std::make_unique<OpenSong> (std::move (*created), AudioBinding { &engine, {}, {} });
        openSong->setTranscribing (true);
    }

    ~LoopRenderFixture() { openSong.reset(); }

    // Polls, as the window's timer does, until `done` holds.
    template <typename Done>
    void pollUntil (Done done)
    {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds (30);
        while (! done())
        {
            CHECK (openSong->poll().empty());
            if (std::chrono::steady_clock::now() > deadline)
                FAIL ("the Practice Speed renders did not get there");
            std::this_thread::sleep_for (std::chrono::milliseconds (5));
        }
    }

    // Plays `planSamples` of output from Song time `from` (the engine's own transport, so a
    // start outside the Loop stays there) and returns the left channel.
    std::vector<float> playFrom (std::int64_t from, int planSamples)
    {
        engine.seek (from);
        engine.play();
        const auto left = device.render (engine, planSamples / 256).channels[0];
        engine.stop();
        device.render (engine, 1);
        return left;
    }

    int rendersInFolder() const { return openSong->listening().renders().practiceFolder().findChildFiles (juce::File::findFiles, false).size(); }
};

std::int64_t peakIndex (const std::vector<float>& samples)
{
    std::int64_t peak = 0;
    for (std::size_t i = 1; i < samples.size(); ++i)
        if (std::abs (samples[i]) > std::abs (samples[(std::size_t) peak]))
            peak = (std::int64_t) i;
    return peak;
}

std::vector<nlohmann::json> eventsNamed (const overdub::core::ScopedLogCapture& capture, const std::string& name)
{
    std::vector<nlohmann::json> named;
    for (const auto& event : capture.events())
        if (event.value ("event", "") == name)
            named.push_back (event);
    return named;
}
} // namespace

TEST_CASE ("while a Loop loops, a Practice Speed change renders the Loop's window before the rest of the Song", "[ui][open_song][practice_speed][loop]")
{
    LoopRenderFixture f;
    REQUIRE (f.openSong->setActiveLoop ("loop", true).has_value());
    overdub::core::ScopedLogCapture capture;

    REQUIRE (f.openSong->setPracticeSpeed (80).has_value());
    f.pollUntil ([&] { return ! f.openSong->listening().renders().isPracticeRendering(); });

    const auto started = eventsNamed (capture, "song.practice_speed_started");
    REQUIRE (started.size() == 2);
    CHECK (started[0].at ("scope") == "loop");
    CHECK (started[0].at ("window_start") == windowStart);
    CHECK (started[0].at ("window_end") == windowEnd);
    CHECK (started[0].at ("speed_percent") == 80);
    CHECK (started[1].at ("scope") == "song");
    // The Loop is heard at the new speed once its renders are there, before the Song's start.
    std::vector<std::string> order;
    for (const auto& event : capture.events())
        if (const auto name = event.value ("event", ""); name == "song.practice_speed_started" || name == "ui.practice_speed_heard")
            order.push_back (name == "ui.practice_speed_heard" ? "heard" : event.at ("scope").get<std::string>());
    CHECK (order == std::vector<std::string> { "loop", "heard", "song" });
    // Once the whole Song is rendered, its render replaces the Loop's.
    CHECK (f.rendersInFolder() == 1);
}

TEST_CASE ("a Loop's audio at the new speed is heard before the whole Song is rendered, at the same Song time", "[ui][open_song][practice_speed][loop]")
{
    LoopRenderFixture f;
    REQUIRE (f.openSong->setActiveLoop ("loop", true).has_value());

    REQUIRE (f.openSong->setPracticeSpeed (80).has_value());
    f.pollUntil ([&] { return f.openSong->listening().renders().heardPracticeSpeed() == 80; });

    // The whole Song's render runs: only poll() ends it, so it still runs while this plays.
    REQUIRE (f.openSong->listening().renders().isPracticeRendering());
    const auto inLoop = f.playFrom (loopStart, 40960);
    // ADR 0005: the impulse at Song time 4.5 s is heard within 1 ms of 0.5 s / 0.8 after 4 s.
    const auto loopRenderPeak = peakIndex (inLoop);
    CHECK_THAT ((double) loopRenderPeak, WithinAbs ((loopClickAt - loopStart) / 0.8, 48.0));
    // Outside the window the source plays silent until its whole render is there.
    CHECK (nonZeroSamples (f.playFrom (0, 64000)).empty());

    f.pollUntil ([&] { return ! f.openSong->listening().renders().isPracticeRendering(); });
    CHECK (f.openSong->listening().renders().heardPracticeSpeed() == 80);
    // The whole render plays the Loop's impulse at the same Song time as the Loop's render did.
    CHECK_THAT ((double) peakIndex (f.playFrom (loopStart, 40960)), WithinAbs ((double) loopRenderPeak, 48.0));
    CHECK_THAT ((double) peakIndex (f.playFrom (0, 64000)), WithinAbs (earlyClickAt / 0.8, 48.0));
}

TEST_CASE ("outside a Loop, with looping off or outside Transcribe mode, the whole Song renders as before", "[ui][open_song][practice_speed][loop]")
{
    LoopRenderFixture f;
    const auto looping = GENERATE (false, true);
    REQUIRE (f.openSong->setActiveLoop ("loop", looping).has_value());
    if (looping)
        f.openSong->setTranscribing (false); // the Loop is not heard outside Transcribe mode
    overdub::core::ScopedLogCapture capture;

    REQUIRE (f.openSong->setPracticeSpeed (80).has_value());
    f.pollUntil ([&] { return ! f.openSong->listening().renders().isPracticeRendering(); });

    const auto started = eventsNamed (capture, "song.practice_speed_started");
    REQUIRE (started.size() == 1);
    CHECK (started[0].at ("scope") == "song");
    CHECK (f.openSong->listening().renders().heardPracticeSpeed() == 80);
    CHECK (f.rendersInFolder() == 1);
}

TEST_CASE ("turning looping on while the whole Song renders renders the Loop's window first", "[ui][open_song][practice_speed][loop]")
{
    LoopRenderFixture f { 80 };
    REQUIRE (f.openSong->setActiveLoop ("loop", false).has_value());
    REQUIRE (f.openSong->setPracticeSpeed (80).has_value());
    REQUIRE (f.openSong->listening().renders().isPracticeRendering());
    overdub::core::ScopedLogCapture capture;

    // The Loop is drilled at 80% too: looping changes only what renders first.
    REQUIRE (f.openSong->setLooping (true).has_value());
    f.pollUntil ([&] { return ! f.openSong->listening().renders().isPracticeRendering(); });

    CHECK (eventsNamed (capture, "song.practice_speed_cancelled").size() == 1);
    const auto started = eventsNamed (capture, "song.practice_speed_started");
    REQUIRE (started.size() == 2);
    CHECK (started[0].at ("scope") == "loop");
    CHECK (started[1].at ("scope") == "song");
    CHECK (f.openSong->listening().renders().heardPracticeSpeed() == 80);
}
