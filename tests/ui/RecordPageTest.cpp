#include "ui/RecordPage.h"

#include "song/TakeEdits.h"
#include "ui/MainWindowFixture.h"

#include <catch2/catch_test_macros.hpp>

// Record mode complete (issue #32, ADR 0005 UI amendment) through the real window: the Passes
// list, the pre-roll picker, the look-ahead strip and the whole-Song overview. Asserts on the
// Song, the playhead, the history, and what the strip and the clock are given to show.
using overdub::song::PreRoll;
using overdub::song::SampleCount;
using namespace overdub::ui;
using namespace overdub::ui::testing;
namespace song = overdub::song;

namespace
{
constexpr SampleCount rate = 48000;

// The 3-Track fixture with an 8 s Band Mix, in Record mode.
struct RecordModeFixture : MainWindowFixture
{
    RecordModeFixture()
        : MainWindowFixture (song::testing::FixtureSongBuilder {}
                                 .backingTrack ("Band Mix", 8.0, 220.0)
                                 .track ("Guitar", true, { 0 })
                                 .track ("Vocals", true, { 1 })
                                 .build())
    {
        window->tick();
        REQUIRE (isShown (page()));
    }

    RecordPage& page() { return window->songView()->recordPage; }

    void recordPass (SampleCount from, int blocks)
    {
        openSong->seek (from);
        runBlocks (0);
        REQUIRE (pressKey (*window, 'r'));
        runBlocks (blocks);
        REQUIRE (pressKey (*window, juce::KeyPress::spaceKey));
        runBlocks (1);
    }
};
} // namespace

TEST_CASE ("Record mode shows the stage, the Passes list, the look-ahead strip and the overview", "[ui][record_page]")
{
    RecordModeFixture f;
    for (const auto* component : std::initializer_list<const juce::Component*> { &f.page().preRollPicker, &f.page().passesList, &f.page().lookAheadStrip, &f.page().overview })
        CHECK (isShown (*component));
    CHECK (f.page().passesList.numRows() == 0);
    CHECK (f.page().passesList.heading.getText() == "No Passes yet: press R to record one.");

    // The strip: one column per pixel of its waveform, the window around the playhead.
    const auto& view = f.page().lookAheadStrip.currentView();
    CHECK ((int) view.columns.size() == f.page().lookAheadStrip.waveformBounds().getWidth());
    CHECK (view.window == lookAheadWindow (0, (int) rate));
    CHECK (view.countdown == "Silence in 8.0 s");
}

TEST_CASE ("clicking a Pass in the list moves the playhead to its start", "[ui][record_page]")
{
    RecordModeFixture f;
    f.recordPass (rate / 2, 100);
    f.recordPass (2 * rate, 100);
    REQUIRE (f.page().passesList.numRows() == 2);
    auto& oldest = *f.page().passesList.rowComponents[1];
    CHECK (oldest.title.getText().startsWith ("Pass 1"));
    CHECK (oldest.status.getText() == "heard");

    f.openSong->seek (5 * rate);
    f.runBlocks (0);
    click (oldest);
    f.runBlocks (0);
    CHECK (f.openSong->position() == rate / 2);

    click (*f.page().passesList.rowComponents[0]);
    f.runBlocks (0);
    CHECK (f.openSong->position() == 2 * rate);
}

TEST_CASE ("Bring back in Record's Passes list is one undo step", "[ui][record_page]")
{
    RecordModeFixture f;
    f.recordPass (0, 100);
    f.recordPass (0, 100); // covers Pass 1 completely
    auto& oldest = *f.page().passesList.rowComponents[1];
    REQUIRE (oldest.status.getText() == "not heard");

    click (oldest.bringBackButton);
    f.runBlocks (0);
    CHECK (f.openSong->package().undoLabel() == "Bring back Pass 1");
    CHECK (song::passHeardStatus (f.song(), f.song().passes[0].id) == song::HeardStatus::heard);
    CHECK (f.page().passesList.rowComponents[1]->status.getText() == "heard");
}

TEST_CASE ("clicking the overview moves the playhead there", "[ui][record_page]")
{
    RecordModeFixture f;
    auto& overview = f.page().overview;
    const auto x = overview.getWidth() / 4;
    const auto expected = overviewSampleAt (x, overview.getWidth(), overview.length());
    REQUIRE (expected > 0);

    clickAt (overview, { x, overview.getHeight() / 2 });
    f.runBlocks (0);
    CHECK (f.openSong->position() == expected);
    CHECK (f.page().lookAheadStrip.currentView().window == lookAheadWindow (expected, (int) rate));
}

TEST_CASE ("changing pre-roll in Record mode writes the Song without a history step", "[ui][record_page][pre_roll]")
{
    RecordModeFixture f;
    const auto stepBefore = f.openSong->package().undoLabel();
    const auto canUndoBefore = f.openSong->package().canUndo();

    choose (f.page().preRollPicker, preRollChoiceIndex (PreRoll::twoSeconds));
    CHECK (f.song().settings.preRoll == PreRoll::twoSeconds);
    CHECK (f.openSong->package().undoLabel() == stepBefore);
    CHECK (f.openSong->package().canUndo() == canUndoBefore);
    CHECK (f.openSong->package().hasPendingWrite());
    CHECK (f.window->topBar.undoButton.getButtonText() == juce::String (undoButtonText (stepBefore)));

    // Undo leaves the pre-roll alone: it is outside history.
    REQUIRE (pressKey (*f.window, 'z', juce::ModifierKeys::commandModifier));
    CHECK (f.song().settings.preRoll == PreRoll::twoSeconds);
    CHECK (f.page().preRollPicker.getSelectedItemIndex() == preRollChoiceIndex (PreRoll::twoSeconds));
}

TEST_CASE ("during pre-roll the clock and the strip count down to the record point, then the Pass is tinted", "[ui][record_page][pre_roll]")
{
    RecordModeFixture f;
    choose (f.page().preRollPicker, preRollChoiceIndex (PreRoll::twoSeconds));
    f.openSong->seek (3 * rate);
    f.runBlocks (0);

    REQUIRE (pressKey (*f.window, 'r'));
    f.runBlocks (0);
    CHECK_FALSE (f.page().preRollPicker.isEnabled());
    CHECK (f.window->topBar.clock.statusText() == juce::String::fromUTF8 ("PRE-ROLL \xC2\xB7 2.0 s"));

    // 75 blocks of 256 = 0.4 s: 1.6 s to go.
    f.runBlocks (75);
    CHECK (f.openSong->position() == rate + 75 * 256);
    CHECK (f.window->topBar.clock.statusText() == juce::String::fromUTF8 ("PRE-ROLL \xC2\xB7 1.6 s"));
    const auto& preRolling = f.page().lookAheadStrip.currentView();
    CHECK (preRolling.recordPoint == 3 * rate);
    CHECK (preRolling.preRollText == "Pass starts in 1.6 s");
    CHECK_FALSE (preRolling.passStart.has_value());

    f.runBlocks (400);
    CHECK (f.window->topBar.clock.statusText() == juce::String::fromUTF8 ("REC \xC2\xB7 PASS 1"));
    const auto& recording = f.page().lookAheadStrip.currentView();
    CHECK (recording.passStart == 3 * rate);
    CHECK_FALSE (recording.recordPoint.has_value());
    CHECK (recording.preRollText.empty());

    REQUIRE (pressKey (*f.window, juce::KeyPress::spaceKey));
    f.runBlocks (1);
    REQUIRE (f.song().passes.size() == 1);
    CHECK (f.song().passes[0].startSample == 3 * rate);
    CHECK (f.page().preRollPicker.isEnabled());
}
