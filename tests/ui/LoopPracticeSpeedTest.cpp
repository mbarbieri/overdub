#include "ui/LoopViews.h"

#include "core/Log.h"
#include "song/LoopEdits.h"
#include "ui/MainWindowFixture.h"
#include "ui/UiHarness.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <string>

using namespace overdub;
using namespace overdub::ui;
using namespace overdub::ui::testing;

// Each Loop remembers its Practice Speed (issue #230, ADR 0010): a new Loop takes the current
// Practice Speed, activating it in Transcribe mode brings that speed back, changing the speed while
// it loops changes the Loop's (outside history), leaving Transcribe mode returns to 100% and coming
// back reapplies the looping Loop's speed. Driven through MainComponent::run and keys on the fixture
// Song (a 2 s Backing Track "Band Mix" and the empty Tracks Guitar and Vocals).
namespace
{
IntentResult runControl (MainWindowFixture& f, const Intent& intent)
{
    return f.window->run (intent, Origin::control);
}

void run (MainWindowFixture& f, SongIntent intent)
{
    REQUIRE (runControl (f, std::move (intent)).has_value());
}

void showMode (MainWindowFixture& f, Mode mode)
{
    REQUIRE (runControl (f, WindowIntent { intent::ShowMode { mode } }).has_value());
}

void setSpeed (MainWindowFixture& f, int speedPercent)
{
    run (f, SongIntent { intent::SetPracticeSpeed { speedPercent } });
}

song::Id addLoop (MainWindowFixture& f, song::SampleCount start, song::SampleCount end)
{
    const auto added = runControl (f, SongIntent { intent::AddLoop { start, end } });
    REQUIRE (added.has_value());
    REQUIRE (added->createdId.has_value());
    return *added->createdId;
}

int loopSpeed (MainWindowFixture& f, const song::Id& loopId)
{
    const auto* loop = song::findLoop (f.song(), loopId);
    REQUIRE (loop != nullptr);
    return loop->practiceSpeedPercent;
}

void press (MainWindowFixture& f, char character)
{
    f.window->keyPressed (juce::KeyPress ((int) character, {}, (juce::juce_wchar) character));
}

void reopenSong (MainWindowFixture& f)
{
    const auto location = f.openSong->package().location();
    f.window->setOpenSong (nullptr);
    f.openSong.reset();
    auto reopened = song::SongPackage::open (location);
    REQUIRE (reopened.has_value());
    f.openSong = std::make_unique<OpenSong> (std::move (reopened->package), AudioBinding { &f.engine, {}, { "In 1", "In 2" } });
    f.window->setOpenSong (f.openSong.get());
}

int countEvents (const core::ScopedLogCapture& capture, const std::string& name)
{
    int count = 0;
    for (const auto& event : capture.events())
        if (event.value ("event", "") == name)
            ++count;
    return count;
}
} // namespace

TEST_CASE ("a new Loop takes the current Practice Speed, and activating a Loop in Transcribe mode sets the Practice Speed to it", "[ui][loops][practice_speed]")
{
    MainWindowFixture f;
    showMode (f, Mode::transcribe);
    setSpeed (f, 40);
    const auto solo = addLoop (f, 24000, 48000);
    CHECK (loopSpeed (f, solo) == 40);
    CHECK (f.openSong->listening().practiceSpeed() == 40);

    // Not looping: the speed is the Practice Speed's alone.
    run (f, SongIntent { intent::SetLooping { false } });
    setSpeed (f, 75);
    CHECK (loopSpeed (f, solo) == 40);
    const auto riff = addLoop (f, 60000, 84000);
    CHECK (loopSpeed (f, riff) == 75);
    CHECK (f.openSong->listening().practiceSpeed() == 75);

    run (f, SongIntent { intent::SelectLoop { solo } });
    CHECK (f.openSong->listening().practiceSpeed() == 40);
    run (f, SongIntent { intent::SelectAdjacentLoop { Direction::forward } });
    CHECK (f.song().activeLoopId == riff);
    CHECK (f.openSong->listening().practiceSpeed() == 75);
    CHECK (loopSpeed (f, solo) == 40);
    CHECK (loopSpeed (f, riff) == 75);

    // L turning looping back on is the Loop heard again.
    run (f, SongIntent { intent::SetLooping { false } });
    setSpeed (f, 100);
    run (f, SongIntent { intent::SetLooping { true } });
    CHECK (f.openSong->listening().practiceSpeed() == 75);

    // In Edit a click only selects a Loop: the Practice Speed stays at 100%.
    showMode (f, Mode::edit);
    run (f, SongIntent { intent::SelectLoop { solo } });
    CHECK (f.openSong->listening().practiceSpeed() == 100);
    const auto verse = addLoop (f, 96000, 120000);
    CHECK (loopSpeed (f, verse) == 100);
    CHECK (loopSpeed (f, solo) == 40);
}

TEST_CASE ("while a Loop loops in Transcribe mode the field, H, - and = change its speed too, outside history", "[ui][loops][practice_speed][keys]")
{
    MainWindowFixture f;
    showMode (f, Mode::transcribe);
    const auto loopId = addLoop (f, 24000, 48000);
    CHECK (loopSpeed (f, loopId) == 100);
    const auto undoLabel = f.openSong->package().undoLabel();
    const core::ScopedLogCapture capture;

    auto& field = f.window->songView()->transcribePage.column.practiceSpeedField;
    field.setText ("60", juce::sendNotificationSync);
    CHECK (f.openSong->listening().practiceSpeed() == 60);
    CHECK (loopSpeed (f, loopId) == 60);
    press (f, 'h');
    CHECK (f.openSong->listening().practiceSpeed() == 50);
    CHECK (loopSpeed (f, loopId) == 50);
    press (f, '=');
    CHECK (f.openSong->listening().practiceSpeed() == 55);
    CHECK (loopSpeed (f, loopId) == 55);
    press (f, '-');
    press (f, '-');
    CHECK (f.openSong->listening().practiceSpeed() == 45);
    CHECK (loopSpeed (f, loopId) == 45);
    CHECK (countEvents (capture, "ui.loop_practice_speed_set") == 5);

    // No undo step: undo still takes back the Loop's making, and undoing a rename keeps the speed.
    CHECK (f.openSong->package().undoLabel() == undoLabel);
    run (f, SongIntent { intent::Edit { intent::RenameLoop { loopId, "Solo" } } });
    setSpeed (f, 70);
    run (f, SongIntent { intent::Undo {} });
    CHECK (song::findLoop (f.song(), loopId)->name == "Loop 1");
    CHECK (loopSpeed (f, loopId) == 70);
    CHECK (f.openSong->listening().practiceSpeed() == 70);
}

TEST_CASE ("leaving Transcribe mode sets the Practice Speed to 100%, and coming back while a Loop loops reapplies its speed", "[ui][loops][practice_speed][transcribe]")
{
    MainWindowFixture f;
    showMode (f, Mode::transcribe);
    setSpeed (f, 40);
    const auto loopId = addLoop (f, 24000, 48000);
    REQUIRE (f.openSong->listening().practiceSpeed() == 40);

    const auto mode = GENERATE (Mode::record, Mode::edit);
    showMode (f, mode);
    CHECK (f.openSong->listening().practiceSpeed() == 100);
    CHECK (loopSpeed (f, loopId) == 40);
    // A speed set outside Transcribe mode is not the Loop's.
    setSpeed (f, 80);
    CHECK (loopSpeed (f, loopId) == 40);

    showMode (f, Mode::transcribe);
    CHECK (f.openSong->listening().practiceSpeed() == 40);

    // Coming back without looping keeps 100%.
    run (f, SongIntent { intent::SetLooping { false } });
    showMode (f, mode);
    CHECK (f.openSong->listening().practiceSpeed() == 100);
    showMode (f, Mode::transcribe);
    CHECK (f.openSong->listening().practiceSpeed() == 100);
    CHECK (loopSpeed (f, loopId) == 40);
}

TEST_CASE ("each Loop's speed is saved in the Song and comes back after reopening it", "[ui][loops][practice_speed][persistence]")
{
    MainWindowFixture f;
    showMode (f, Mode::transcribe);
    setSpeed (f, 40);
    const auto solo = addLoop (f, 24000, 48000);
    run (f, SongIntent { intent::SetLooping { false } });
    setSpeed (f, 75);
    const auto riff = addLoop (f, 60000, 84000);
    REQUIRE (loopSpeed (f, solo) == 40);
    REQUIRE (loopSpeed (f, riff) == 75);
    REQUIRE (f.openSong->package().flush().has_value());

    reopenSong (f);
    CHECK (loopSpeed (f, solo) == 40);
    CHECK (loopSpeed (f, riff) == 75);
    // Reopened in Transcribe mode with the riff looping: its speed.
    CHECK (f.openSong->listening().practiceSpeed() == 75);
    run (f, SongIntent { intent::SelectLoop { solo } });
    CHECK (f.openSong->listening().practiceSpeed() == 40);
    REQUIRE (f.openSong->package().flush().has_value());

    showMode (f, Mode::edit);
    reopenSong (f);
    CHECK (f.openSong->listening().practiceSpeed() == 100);
    CHECK (f.song().activeLoopId == solo);
    showMode (f, Mode::transcribe);
    CHECK (f.openSong->listening().practiceSpeed() == 40);
}

TEST_CASE ("the Loop cards show each Loop's speed", "[ui][loops][practice_speed][transcribe]")
{
    MainWindowFixture f;
    showMode (f, Mode::transcribe);
    setSpeed (f, 40);
    const auto solo = addLoop (f, 24000, 48000);
    run (f, SongIntent { intent::SetLooping { false } });
    setSpeed (f, 75);
    addLoop (f, 60000, 84000);

    auto& list = f.window->songView()->transcribePage.loopList;
    REQUIRE (list.cards.size() == 2);
    CHECK (list.cards[0]->speedText() == "40%");
    CHECK (list.cards[1]->speedText() == "75%");

    run (f, SongIntent { intent::SelectLoop { solo } });
    press (f, 'h');
    CHECK (list.cards[0]->speedText() == "50%");
    CHECK (list.cards[1]->speedText() == "75%");
}
