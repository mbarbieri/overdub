#include "ui/MainComponent.h"

#include "core/Log.h"
#include "ui/MainWindowFixture.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <chrono>
#include <string>
#include <vector>

using namespace overdub;
using namespace overdub::ui;
using namespace overdub::ui::testing;

namespace
{
constexpr song::SampleCount sampleRate = 48000;

IntentResult runFromSocket (MainWindowFixture& f, SongIntent intent)
{
    return f.window->run (std::move (intent), Origin::control);
}

// Records a Pass from `from` for `blocks` device blocks, then stops it and lets the window finish it.
void recordPass (MainWindowFixture& f, song::SampleCount from, int blocks)
{
    REQUIRE (runFromSocket (f, intent::Seek { from }).has_value());
    REQUIRE (runFromSocket (f, intent::Record {}).has_value());
    f.runBlocks (blocks);
    REQUIRE (runFromSocket (f, intent::PlayOrStop {}).has_value());
    f.runBlocks (1);
    REQUIRE_FALSE (f.openSong->isRecording());
}

std::vector<nlohmann::json> eventsNamed (const core::ScopedLogCapture& capture, const std::string& name)
{
    std::vector<nlohmann::json> found;
    for (const auto& event : capture.events())
        if (event.at ("event") == name)
            found.push_back (event);
    return found;
}

std::vector<song::SampleCount> passStarts (const MainWindowFixture& f)
{
    std::vector<song::SampleCount> starts;
    for (const auto& pass : f.song().passes)
        starts.push_back (pass.startSample);
    return starts;
}
} // namespace

TEST_CASE ("record_again starts a new Pass at the most recent Pass's record point, wherever the playhead is", "[ui][intent][record_again]")
{
    MainWindowFixture f;
    recordPass (f, sampleRate / 2, 20);
    REQUIRE (runFromSocket (f, intent::Seek { 3 * sampleRate }).has_value());
    core::ScopedLogCapture capture;

    REQUIRE (runFromSocket (f, intent::RecordAgain {}).has_value());
    f.runBlocks (20);
    REQUIRE (runFromSocket (f, intent::PlayOrStop {}).has_value());
    f.runBlocks (1);

    CHECK (passStarts (f) == std::vector<song::SampleCount> { sampleRate / 2, sampleRate / 2 });
    const auto started = eventsNamed (capture, "ui.record_again");
    REQUIRE (started.size() == 1);
    CHECK (started[0].at ("record_point") == sampleRate / 2);
    CHECK (started[0].at ("pass_id") == f.song().passes[0].id);
}

TEST_CASE ("record_again during a Pass keeps its Takes and starts the next Pass at the same record point", "[ui][intent][record_again]")
{
    MainWindowFixture f;
    REQUIRE (runFromSocket (f, intent::Seek { sampleRate }).has_value());
    REQUIRE (runFromSocket (f, intent::Record {}).has_value());
    f.runBlocks (20);

    REQUIRE (runFromSocket (f, intent::RecordAgain {}).has_value());
    f.runBlocks (1);

    CHECK (f.openSong->isRecording());
    CHECK (passStarts (f) == std::vector<song::SampleCount> { sampleRate });
    const auto firstPassTakes = f.song().takes.size();
    CHECK (firstPassTakes == 2); // Guitar and Vocals
    f.runBlocks (20);
    CHECK (f.openSong->runningPassStart() == sampleRate);
    REQUIRE (runFromSocket (f, intent::PlayOrStop {}).has_value());
    f.runBlocks (1);
    CHECK (passStarts (f) == std::vector<song::SampleCount> { sampleRate, sampleRate });
    CHECK (f.song().takes.size() == 2 * firstPassTakes);
}

TEST_CASE ("record_again with pre-roll plays from the pre-roll before the record point", "[ui][intent][record_again][pre_roll]")
{
    MainWindowFixture f;
    recordPass (f, 3 * sampleRate, 20);
    REQUIRE (runFromSocket (f, intent::SetPreRoll { song::PreRoll::oneSecond }).has_value());

    REQUIRE (runFromSocket (f, intent::RecordAgain {}).has_value());

    CHECK (f.openSong->preRollRecordPoint() == 3 * sampleRate);
    CHECK (f.openSong->position() == 2 * sampleRate);
    f.runBlocks (sampleRate / deviceBlockSize + 2); // past the record point
    CHECK (f.openSong->runningPassStart() == 3 * sampleRate);
    REQUIRE (runFromSocket (f, intent::PlayOrStop {}).has_value());
    f.runBlocks (1);
}

TEST_CASE ("record_again in a Song with no Pass does nothing and says why", "[ui][intent][record_again]")
{
    MainWindowFixture f;

    const auto result = runFromSocket (f, intent::RecordAgain {});

    REQUIRE_FALSE (result.has_value());
    CHECK (result.error().code == "ui.no_pass_to_record_again");
    CHECK_FALSE (f.openSong->isRecording());
}

TEST_CASE ("⇧R runs record_again in both modes", "[ui][intent][record_again][keys]")
{
    MainWindowFixture f;
    recordPass (f, sampleRate, 20);
    const auto mode = GENERATE (Mode::record, Mode::edit);
    REQUIRE (f.window->run (WindowIntent { intent::ShowMode { mode } }, Origin::control).has_value());
    REQUIRE (runFromSocket (f, intent::Seek { 0 }).has_value());

    REQUIRE (pressKey (*f.window, 'R', juce::ModifierKeys::shiftModifier));
    f.runBlocks (5);

    CHECK (f.openSong->runningPassStart() == sampleRate);
    REQUIRE (runFromSocket (f, intent::PlayOrStop {}).has_value());
    f.runBlocks (1);
}

namespace
{
constexpr std::chrono::milliseconds doubleTap { 400 };

// The window's clock and double-Space interval, set by the test: Spaces are pressed at `now`.
struct SpaceTaps
{
    MainWindowFixture& f;
    song::SongPackage::Clock::time_point now {};

    explicit SpaceTaps (MainWindowFixture& fixture) : f (fixture)
    {
        f.window->clock = [this] { return now; };
        f.window->doubleTapInterval = [] { return doubleTap; };
    }

    void space (std::chrono::milliseconds after)
    {
        now += after;
        REQUIRE (pressKey (*f.window, juce::KeyPress::spaceKey));
    }

    // Device blocks, then one window poll at `now` advanced by `after`.
    void wait (std::chrono::milliseconds after, int blocks = 1)
    {
        now += after;
        f.device.run (f.engine, blocks);
        f.window->tick (now);
    }
};
} // namespace

TEST_CASE ("in Record mode, two Spaces during a Pass stop it at once and record again from its record point", "[ui][keys][record_again]")
{
    MainWindowFixture f;
    SpaceTaps taps { f };
    REQUIRE (runFromSocket (f, intent::Seek { sampleRate }).has_value());
    REQUIRE (runFromSocket (f, intent::Record {}).has_value());
    taps.wait (std::chrono::milliseconds (0), 20);

    taps.space (std::chrono::milliseconds (0));
    taps.wait (std::chrono::milliseconds (0));
    CHECK_FALSE (f.openSong->isPlaying()); // the first Space stops at once
    taps.wait (std::chrono::milliseconds (100));
    CHECK (passStarts (f) == std::vector<song::SampleCount> { sampleRate });
    taps.space (std::chrono::milliseconds (100));
    taps.wait (std::chrono::milliseconds (0), 20);

    CHECK (f.openSong->runningPassStart() == sampleRate);
    REQUIRE (runFromSocket (f, intent::PlayOrStop {}).has_value());
    taps.wait (std::chrono::milliseconds (0));
    CHECK (passStarts (f) == std::vector<song::SampleCount> { sampleRate, sampleRate });
}

TEST_CASE ("in Record mode, two Spaces while playing or stopped record again from the last record point", "[ui][keys][record_again]")
{
    MainWindowFixture f;
    recordPass (f, sampleRate, 20);
    SpaceTaps taps { f };
    REQUIRE (runFromSocket (f, intent::Seek { 3 * sampleRate }).has_value());
    SECTION ("playing")
    {
        REQUIRE (runFromSocket (f, intent::PlayOrStop {}).has_value());
        taps.wait (std::chrono::milliseconds (0), 5);
        taps.space (std::chrono::milliseconds (0));
        taps.wait (std::chrono::milliseconds (0));
        CHECK_FALSE (f.openSong->isPlaying());
    }
    SECTION ("stopped")
    {
        taps.space (std::chrono::milliseconds (0));
        taps.wait (std::chrono::milliseconds (0));
        CHECK_FALSE (f.openSong->isPlaying());
    }

    taps.space (std::chrono::milliseconds (300));
    taps.wait (std::chrono::milliseconds (600), 5);

    CHECK (f.openSong->runningPassStart() == sampleRate);
    REQUIRE (runFromSocket (f, intent::PlayOrStop {}).has_value());
    taps.wait (std::chrono::milliseconds (0));
}

TEST_CASE ("in Record mode, a single Space while stopped plays only once the double-Space interval has passed", "[ui][keys][record_again]")
{
    MainWindowFixture f;
    SpaceTaps taps { f };

    taps.space (std::chrono::milliseconds (0));
    taps.wait (std::chrono::milliseconds (399));
    taps.wait (std::chrono::milliseconds (0));
    CHECK_FALSE (f.openSong->isPlaying());
    taps.wait (std::chrono::milliseconds (2));
    taps.wait (std::chrono::milliseconds (0));
    CHECK (f.openSong->isPlaying());
    CHECK_FALSE (f.openSong->isRecording());

    // Playing, one Space stops at once; a Space past the interval is a new single Space.
    taps.space (std::chrono::milliseconds (0));
    taps.wait (std::chrono::milliseconds (0));
    CHECK_FALSE (f.openSong->isPlaying());
    taps.space (std::chrono::milliseconds (401));
    taps.wait (std::chrono::milliseconds (401));
    taps.wait (std::chrono::milliseconds (0));
    CHECK (f.openSong->isPlaying());
    CHECK (f.song().passes.empty());
}

TEST_CASE ("in Edit mode, two quick Spaces play and then stop", "[ui][keys][record_again]")
{
    MainWindowFixture f;
    recordPass (f, sampleRate, 20);
    SpaceTaps taps { f };
    REQUIRE (f.window->run (WindowIntent { intent::ShowMode { Mode::edit } }, Origin::control).has_value());

    taps.space (std::chrono::milliseconds (0));
    taps.wait (std::chrono::milliseconds (0));
    CHECK (f.openSong->isPlaying());
    taps.space (std::chrono::milliseconds (100));
    taps.wait (std::chrono::milliseconds (600));

    CHECK_FALSE (f.openSong->isPlaying());
    CHECK_FALSE (f.openSong->isRecording());
    CHECK (f.song().passes.size() == 1);
}

TEST_CASE ("in Record mode, a Space after another gesture is a first Space again", "[ui][keys][record_again]")
{
    MainWindowFixture f;
    SpaceTaps taps { f };
    REQUIRE (pressKey (*f.window, 'r'));
    taps.wait (std::chrono::milliseconds (0), 5);
    taps.space (std::chrono::milliseconds (0)); // stops Pass 1
    taps.wait (std::chrono::milliseconds (50));
    REQUIRE (pressKey (*f.window, 'r'));
    taps.wait (std::chrono::milliseconds (50), 5);

    taps.space (std::chrono::milliseconds (100)); // stops Pass 2, not Record Again
    taps.wait (std::chrono::milliseconds (600));

    CHECK_FALSE (f.openSong->isRecording());
    CHECK (f.song().passes.size() == 2);
}

TEST_CASE ("in Record mode, a second Space while the stopped first Pass still saves records again from that Pass", "[ui][keys][record_again]")
{
    MainWindowFixture f;
    SpaceTaps taps { f };
    REQUIRE (runFromSocket (f, intent::Seek { sampleRate }).has_value());
    REQUIRE (runFromSocket (f, intent::Record {}).has_value());
    f.device.run (f.engine, 20);

    taps.space (std::chrono::milliseconds (0));
    f.device.run (f.engine, 1); // the audio thread ends the Pass; no poll has finished it yet
    REQUIRE (f.openSong->isRecording());
    taps.space (std::chrono::milliseconds (100));
    taps.wait (std::chrono::milliseconds (0)); // finishes Pass 1 and starts the next
    taps.wait (std::chrono::milliseconds (0), 20);

    CHECK (f.openSong->runningPassStart() == sampleRate);
    REQUIRE (runFromSocket (f, intent::PlayOrStop {}).has_value());
    taps.wait (std::chrono::milliseconds (0));
    CHECK (passStarts (f) == std::vector<song::SampleCount> { sampleRate, sampleRate });
}
