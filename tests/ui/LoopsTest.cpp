#include "ui/LoopViews.h"

#include "core/Log.h"
#include "song/LoopEdits.h"
#include "ui/MainWindowFixture.h"
#include "ui/MarkerStrip.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <string>
#include <vector>

using namespace overdub;
using namespace overdub::ui;
using namespace overdub::ui::testing;

// Loops (issue #228, ADR 0010): making one by dragging on the Loop/Marker strip or with [ and ],
// resizing, renaming and deleting it, Transcribe mode's Loop cards, activating a Loop (click, card,
// option-arrows, option-1..9), L, and looping playback in Transcribe mode only. Driven through
// MainComponent::run, keys and clicks on the fixture Song (a 2 s Backing Track "Band Mix" and the
// empty Tracks Guitar and Vocals).
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

song::Id addLoop (MainWindowFixture& f, song::SampleCount start, song::SampleCount end)
{
    const auto added = runControl (f, SongIntent { intent::AddLoop { start, end } });
    REQUIRE (added.has_value());
    REQUIRE (added->createdId.has_value());
    return *added->createdId;
}

const song::Loop& loopWithId (MainWindowFixture& f, const song::Id& loopId)
{
    const auto* loop = song::findLoop (f.song(), loopId);
    REQUIRE (loop != nullptr);
    return *loop;
}

MarkerStrip& stripOf (MainWindowFixture& f, Mode mode)
{
    auto& view = *f.window->songView();
    switch (mode)
    {
        case Mode::record: return view.recordPage.markerStrip;
        case Mode::edit: return view.editPage.timeline.markerStrip;
        case Mode::transcribe: return view.transcribePage.markerStrip;
    }
    return view.recordPage.markerStrip;
}

StripScale scaleOf (MainWindowFixture& f, Mode mode)
{
    auto& view = *f.window->songView();
    return stripScaleFor (mode == Mode::edit ? view.editPage.timeline.view() : view.transcribePage.view());
}

LoopBar& barOf (MarkerStrip& strip, const song::Id& loopId)
{
    for (auto& bar : strip.bars)
        if (bar->loopId() == loopId)
            return *bar;
    FAIL ("no bar for Loop " << loopId);
    return *strip.bars.front();
}

void pressKey (MainWindowFixture& f, int keyCode, juce::juce_wchar character = 0, juce::ModifierKeys modifiers = {})
{
    f.window->keyPressed (juce::KeyPress (keyCode, modifiers, character));
}

void pressOption (MainWindowFixture& f, int keyCode)
{
    pressKey (f, keyCode, 0, juce::ModifierKeys (juce::ModifierKeys::altModifier));
}

std::vector<nlohmann::json> eventsNamed (const core::ScopedLogCapture& capture, const std::string& name)
{
    std::vector<nlohmann::json> found;
    for (const auto& event : capture.events())
        if (event.value ("event", "") == name)
            found.push_back (event);
    return found;
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
} // namespace

TEST_CASE ("dragging on an empty part of the strip makes Loop N over that range and activates it", "[ui][loops]")
{
    MainWindowFixture f;
    const auto mode = GENERATE (Mode::edit, Mode::transcribe);
    showMode (f, mode);
    auto& strip = stripOf (f, mode);
    REQUIRE (strip.isEditable());

    const auto scale = scaleOf (f, mode);
    const core::ScopedLogCapture capture;
    drag (strip, { 20, strip.getHeight() / 2 }, { 70, strip.getHeight() / 2 });

    REQUIRE (f.song().loops.size() == 1);
    const auto& loop = f.song().loops[0];
    CHECK (loop.name == "Loop 1");
    CHECK (loop.start == stripSample (scale, 20));
    CHECK (loop.end == stripSample (scale, 70));
    CHECK (f.openSong->package().undoLabel() == song::addLoopLabel);
    CHECK (f.song().settings.activeLoopId == loop.id);
    CHECK (f.song().settings.looping);
    REQUIRE (eventsNamed (capture, "ui.loop_added").size() == 1);
    CHECK (eventsNamed (capture, "ui.loop_added")[0].at ("loop_id") == loop.id);
    REQUIRE (strip.bars.size() == 1);
    CHECK (strip.bars[0]->getX() == stripX (stripScaleFor (mode == Mode::edit ? f.window->songView()->editPage.timeline.view() : f.window->songView()->transcribePage.view()), loop.start));
    CHECK (strip.bars[0]->isActive());
    if (mode == Mode::edit)
        CHECK (f.window->songView()->selectedLoop() == loop.id);

    // Right to left makes the same range; a second Loop takes the next number.
    drag (strip, { 140, strip.getHeight() / 2 }, { 100, strip.getHeight() / 2 });
    REQUIRE (f.song().loops.size() == 2);
    CHECK (f.song().loops[1].name == "Loop 2");
    CHECK (f.song().loops[1].start < f.song().loops[1].end);
    CHECK (f.song().settings.activeLoopId == f.song().loops[1].id);

    // A click on the empty strip makes no Loop.
    clickAt (strip, { strip.getWidth() - 4, strip.getHeight() / 2 });
    CHECK (f.song().loops.size() == 2);
}

TEST_CASE ("the open bracket marks a pending start at the playhead, the close bracket makes the Loop up to the playhead, and Esc cancels the pending start", "[ui][loops][keys]")
{
    MainWindowFixture f;
    const auto mode = GENERATE (Mode::edit, Mode::transcribe);
    showMode (f, mode);

    run (f, SongIntent { intent::Seek { 12000 } });
    pressKey (f, '[', '[');
    CHECK (f.window->songView()->loopStartPending() == 12000);
    CHECK (f.song().loops.empty());
    run (f, SongIntent { intent::Seek { 36000 } });
    pressKey (f, ']', ']');
    REQUIRE (f.song().loops.size() == 1);
    CHECK (f.song().loops[0].start == 12000);
    CHECK (f.song().loops[0].end == 36000);
    CHECK (f.song().settings.activeLoopId == f.song().loops[0].id);
    CHECK_FALSE (f.window->songView()->loopStartPending().has_value());

    // While listening: [ and ] at the playhead as it moves, without stopping playback.
    run (f, SongIntent { intent::Seek { 0 } });
    run (f, SongIntent { intent::Edit { intent::DeleteLoop { f.song().loops[0].id } } });
    run (f, SongIntent { intent::PlayOrStop {} });
    f.runBlocks (4);
    const auto start = f.openSong->position();
    pressKey (f, '[', '[');
    f.runBlocks (8);
    const auto end = f.openSong->position();
    pressKey (f, ']', ']');
    CHECK (f.openSong->isPlaying());
    REQUIRE (f.song().loops.size() == 1);
    CHECK (f.song().loops[0].start == start);
    CHECK (f.song().loops[0].end == end);
    run (f, SongIntent { intent::PlayOrStop {} });

    // Esc cancels; ] then has no start and makes nothing.
    pressKey (f, '[', '[');
    REQUIRE (f.window->songView()->loopStartPending().has_value());
    pressKey (f, juce::KeyPress::escapeKey);
    CHECK_FALSE (f.window->songView()->loopStartPending().has_value());
    const auto noStart = runControl (f, SongIntent { intent::MarkLoopEnd {} });
    REQUIRE_FALSE (noStart.has_value());
    CHECK (noStart.error().code == "ui.no_loop_start");
    CHECK (f.song().loops.size() == 1);

    // A Loop with no length is refused.
    run (f, SongIntent { intent::MarkLoopStart {} });
    const auto empty = runControl (f, SongIntent { intent::MarkLoopEnd {} });
    REQUIRE_FALSE (empty.has_value());
    CHECK (empty.error().code == "song.loop_empty");
}

TEST_CASE ("Record mode refuses making and editing Loops; its strip shows them read-only", "[ui][loops][availability]")
{
    MainWindowFixture f;
    showMode (f, Mode::edit);
    const auto loopId = addLoop (f, 12000, 24000);
    showMode (f, Mode::record);
    auto& strip = stripOf (f, Mode::record);
    CHECK_FALSE (strip.isEditable());
    REQUIRE (strip.bars.size() == 1);

    const std::vector<Intent> refused {
        SongIntent { intent::AddLoop { 0, 1000 } },
        SongIntent { intent::MarkLoopStart {} },
        SongIntent { intent::MarkLoopEnd {} },
        SongIntent { intent::Edit { intent::ResizeLoop { loopId, 0, 1000 } } },
        SongIntent { intent::Edit { intent::RenameLoop { loopId, "Riff" } } },
        SongIntent { intent::Edit { intent::DeleteLoop { loopId } } },
    };
    for (const auto& intent : refused)
    {
        const auto result = runControl (f, intent);
        REQUIRE_FALSE (result.has_value());
        CHECK (result.error().code == "ui.record_mode");
    }
    pressKey (f, '[', '[');
    CHECK_FALSE (f.window->songView()->loopStartPending().has_value());

    // Drags and double-clicks change nothing.
    drag (strip, { 2, strip.getHeight() / 2 }, { 30, strip.getHeight() / 2 });
    auto& bar = barOf (strip, loopId);
    drag (bar, { 1, bar.getHeight() / 2 }, { 20, bar.getHeight() / 2 });
    doubleClickAt (bar, bar.getLocalBounds().getCentre());
    CHECK_FALSE (isShown (strip.nameField));
    CHECK (f.song().loops == std::vector<song::Loop> { song::Loop { loopId, "Loop 1", 12000, 24000 } });
}

TEST_CASE ("dragging a Loop's edge resizes it, double-clicking renames it and Delete removes the active or selected Loop, each one undo step", "[ui][loops]")
{
    MainWindowFixture f;
    const auto mode = GENERATE (Mode::edit, Mode::transcribe);
    showMode (f, mode);
    const auto loopId = addLoop (f, 24000, 48000);
    auto& strip = stripOf (f, mode);

    // Resize: the end edge 10 pixels right.
    const auto perPixel = (song::SampleCount) scaleOf (f, mode).samplesPerPixel;
    {
        auto& bar = barOf (strip, loopId);
        const auto y = bar.getHeight() / 2;
        REQUIRE (bar.edgeAt (bar.getWidth() - 1) == LoopEdge::end);
        drag (bar, { bar.getWidth() - 1, y }, { bar.getWidth() + 9, y });
    }
    CHECK (loopWithId (f, loopId).start == 24000);
    CHECK (loopWithId (f, loopId).end == 48000 + 10 * perPixel);
    CHECK (f.openSong->package().undoLabel() == song::resizeLoopLabel);
    {
        auto& bar = barOf (strip, loopId);
        const auto y = bar.getHeight() / 2;
        drag (bar, { 0, y }, { -5, y });
    }
    CHECK (loopWithId (f, loopId).start == 24000 - 5 * perPixel);

    // Rename.
    doubleClickAt (barOf (strip, loopId), barOf (strip, loopId).getLocalBounds().getCentre());
    REQUIRE (isShown (strip.nameField));
    CHECK (strip.nameField.getText() == "Loop 1");
    strip.nameField.setText ("Solo", juce::sendNotificationSync);
    CHECK (loopWithId (f, loopId).name == "Solo");
    CHECK (f.openSong->package().undoLabel() == song::renameLoopLabel);
    strip.nameField.hideEditor (true);

    // Delete: in Transcribe the active Loop, in Edit the selected one (a click selects it).
    if (mode == Mode::edit)
        click (barOf (strip, loopId));
    CHECK (f.window->songView()->loopToDelete() == loopId);
    pressKey (f, juce::KeyPress::backspaceKey);
    CHECK (f.song().loops.empty());
    CHECK (f.openSong->package().undoLabel() == song::deleteLoopLabel);
    CHECK_FALSE (f.song().settings.activeLoopId.has_value());

    run (f, SongIntent { intent::Undo {} });
    CHECK (loopWithId (f, loopId).name == "Solo");
    run (f, SongIntent { intent::Undo {} });
    CHECK (loopWithId (f, loopId).name == "Loop 1");
    run (f, SongIntent { intent::Undo {} });
    CHECK (loopWithId (f, loopId).start == 24000);
    run (f, SongIntent { intent::Undo {} });
    CHECK (loopWithId (f, loopId).end == 48000);
    run (f, SongIntent { intent::Undo {} });
    CHECK (f.song().loops.empty());
    for (int step = 0; step < 4; ++step)
        run (f, SongIntent { intent::Redo {} });
    const auto expected = song::Loop { loopId, "Solo", 24000 - 5 * perPixel, 48000 + 10 * perPixel };
    CHECK (f.song().loops == std::vector<song::Loop> { expected });

    reopenSong (f);
    CHECK (f.song().loops == std::vector<song::Loop> { expected });
}

TEST_CASE ("Transcribe mode's left column shows a card per Loop above the Markers, and a card click activates its Loop", "[ui][loops][transcribe]")
{
    MainWindowFixture f;
    showMode (f, Mode::transcribe);
    auto& page = f.window->songView()->transcribePage;
    auto& list = page.loopList;
    CHECK (isShown (list));
    CHECK (isShown (list.hint));
    CHECK (list.getBottom() < page.markerList.getY());
    CHECK (list.getX() == page.markerList.getX());

    const auto late = addLoop (f, 72000, 84000);
    const auto early = addLoop (f, 24000, 48000);
    REQUIRE (list.cards.size() == 2);
    CHECK_FALSE (isShown (list.hint));
    CHECK (list.cards[0]->loopId() == early);
    CHECK (list.cards[1]->loopId() == late);
    CHECK (list.cards[0]->keyText() == juce::String::fromUTF8 ("\xe2\x8c\xa5"
                                                               "1"));
    CHECK (list.cards[1]->keyText() == juce::String::fromUTF8 ("\xe2\x8c\xa5"
                                                               "2"));
    CHECK (loopRangeText (loopWithId (f, early), 48000) == "0.5 s \xe2\x80\x93 1.0 s");
    CHECK (loopLengthText (loopWithId (f, early), 48000) == "0.5 s");
    CHECK (list.cards[0]->isActive()); // just made, so active

    click (*list.cards[1]);
    CHECK (f.song().settings.activeLoopId == late);
    CHECK (f.song().settings.looping);
    CHECK (list.cards[1]->isActive());
    CHECK_FALSE (list.cards[0]->isActive());
}

TEST_CASE ("in Transcribe mode a click, option-arrows and option-1..9 activate a Loop: looping on, zoomed to fit it; L toggles looping; both saved outside history", "[ui][loops][transcribe][keys]")
{
    MainWindowFixture f;
    showMode (f, Mode::transcribe);
    // Longer than the finest zoom's 2 s, so each fits the width.
    const auto first = addLoop (f, 2 * 48000, 4 * 48000);
    const auto second = addLoop (f, 6 * 48000, 12 * 48000);
    const auto third = addLoop (f, 14 * 48000, 16 * 48000);
    auto& page = f.window->songView()->transcribePage;

    click (barOf (page.markerStrip, second));
    CHECK (f.song().settings.activeLoopId == second);
    CHECK (f.song().settings.looping);
    // Zoomed to fit with a small margin: the whole Loop shows, and it fills most of the width.
    const auto view = page.view();
    const auto shownEnd = view.firstSample + view.samplesPerPixel * page.wave.getWidth();
    CHECK (view.firstSample < 6 * 48000);
    CHECK (view.firstSample > 6 * 48000 - 6 * 48000 / 5);
    CHECK (shownEnd > 12 * 48000);
    CHECK (shownEnd < 12 * 48000 + 6 * 48000 / 5);
    CHECK (f.openSong->position() == 6 * 48000);

    // A Loop shorter than the finest zoom shows centred at it.
    const auto shortLoop = addLoop (f, 20 * 48000, 20 * 48000 + 12000);
    const auto zoomedIn = page.view();
    const auto centre = zoomedIn.firstSample + zoomedIn.samplesPerPixel * page.wave.getWidth() / 2;
    CHECK (std::abs (centre - (20 * 48000 + 6000)) <= zoomedIn.samplesPerPixel);
    run (f, SongIntent { intent::Edit { intent::DeleteLoop { shortLoop } } });
    run (f, SongIntent { intent::SelectLoop { second } });
    const auto undoLabel = f.openSong->package().undoLabel();

    pressOption (f, juce::KeyPress::leftKey);
    CHECK (f.song().settings.activeLoopId == first);
    pressOption (f, juce::KeyPress::leftKey); // the first has no previous
    CHECK (f.song().settings.activeLoopId == first);
    pressOption (f, juce::KeyPress::rightKey);
    CHECK (f.song().settings.activeLoopId == second);
    pressOption (f, '3');
    CHECK (f.song().settings.activeLoopId == third);
    pressOption (f, '1');
    CHECK (f.song().settings.activeLoopId == first);
    pressOption (f, '9'); // no ninth Loop
    CHECK (f.song().settings.activeLoopId == first);

    pressKey (f, 'l', 'l');
    CHECK_FALSE (f.song().settings.looping);
    pressKey (f, 'L', 'L');
    CHECK (f.song().settings.looping);
    // Activating and L take no undo step.
    CHECK (f.openSong->package().undoLabel() == undoLabel);

    pressOption (f, '2');
    pressKey (f, 'l', 'l');
    reopenSong (f);
    CHECK (f.song().settings.activeLoopId == second);
    CHECK_FALSE (f.song().settings.looping);

    // Without an active Loop, L cannot turn looping on.
    run (f, SongIntent { intent::Edit { intent::DeleteLoop { second } } });
    const auto refused = runControl (f, SongIntent { intent::SetLooping { true } });
    REQUIRE_FALSE (refused.has_value());
    CHECK (refused.error().code == "song.no_active_loop");
}

TEST_CASE ("while looping in Transcribe mode playback wraps at the Loop's end, and Space starts at its start unless the playhead is inside", "[ui][loops][transcribe]")
{
    MainWindowFixture f;
    showMode (f, Mode::transcribe);
    const auto loopId = addLoop (f, 24000, 30000);
    REQUIRE (f.openSong->heard().loop == engine::LoopRange { 24000, 30000 });

    // From outside the Loop, Space starts at its start.
    run (f, SongIntent { intent::Seek { 60000 } });
    f.runBlocks (1);
    pressKey (f, juce::KeyPress::spaceKey, ' ');
    f.runBlocks (1);
    CHECK (f.openSong->isPlaying());
    // 40 blocks are 10240 samples: past the Loop's 6000 several times, so it wrapped.
    f.runBlocks (39);
    const auto position = f.openSong->position();
    CHECK (position >= 24000);
    CHECK (position < 30000);
    CHECK (position == 24000 + (40 * deviceBlockSize) % 6000);
    run (f, SongIntent { intent::PlayOrStop {} });

    // From inside it, Space starts where the playhead is.
    run (f, SongIntent { intent::Seek { 27000 } });
    f.runBlocks (1);
    run (f, SongIntent { intent::PlayOrStop {} });
    f.runBlocks (1);
    CHECK (f.openSong->position() == 27000 + deviceBlockSize);
    run (f, SongIntent { intent::PlayOrStop {} });

    // Looping off: plays straight on.
    run (f, SongIntent { intent::SetLooping { false } });
    CHECK_FALSE (f.openSong->heard().loop.has_value());
    run (f, SongIntent { intent::SetLooping { true } });
    CHECK (f.openSong->heard().loop.has_value());
    CHECK (loopWithId (f, loopId).start == 24000);
}

TEST_CASE ("Record and Edit never loop; in Record a click on a Loop moves the record point to its start", "[ui][loops][record]")
{
    MainWindowFixture f;
    showMode (f, Mode::transcribe);
    const auto loopId = addLoop (f, 24000, 30000);
    REQUIRE (f.openSong->heard().loop.has_value());

    const auto mode = GENERATE (Mode::edit, Mode::record);
    showMode (f, mode);
    CHECK_FALSE (f.openSong->heard().loop.has_value());
    CHECK (f.song().settings.looping); // kept for when Transcribe mode is back

    run (f, SongIntent { intent::Seek { 0 } });
    click (barOf (stripOf (f, mode), loopId));
    CHECK (f.openSong->position() == 24000);
    CHECK (f.song().settings.activeLoopId == loopId);
    run (f, SongIntent { intent::PlayOrStop {} });
    f.runBlocks (40);
    CHECK (f.openSong->position() == 24000 + 40 * deviceBlockSize);
    run (f, SongIntent { intent::PlayOrStop {} });
    f.runBlocks (1); // the transport has stopped before record() looks

    if (mode == Mode::record)
    {
        CHECK_FALSE (f.window->songView()->selectedLoop().has_value());
        CHECK_FALSE (f.window->songView()->loopToDelete().has_value());
        run (f, SongIntent { intent::Seek { 0 } });
        click (barOf (stripOf (f, mode), loopId));
        REQUIRE (f.openSong->position() == 24000);
        const auto recorded = runControl (f, SongIntent { intent::Record {} });
        REQUIRE (recorded.has_value());
        f.runBlocks (8);
        CHECK (f.openSong->runningPassStart() == 24000);
        run (f, SongIntent { intent::PlayOrStop {} });
        f.runBlocks (2);
    }
    else
    {
        CHECK (f.window->songView()->selectedLoop() == loopId);
    }

    showMode (f, Mode::transcribe);
    CHECK (f.openSong->heard().loop == engine::LoopRange { 24000, 30000 });
}
