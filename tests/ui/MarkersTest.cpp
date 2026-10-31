#include "ui/MarkerStrip.h"

#include "core/Log.h"
#include "song/MarkerEdits.h"
#include "ui/MainWindowFixture.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <string>
#include <vector>

using namespace overdub;
using namespace overdub::ui;
using namespace overdub::ui::testing;

// Markers (issue #227, ADR 0010): the Loop/Marker strip above the ruler in every mode, K, renaming,
// moving and deleting a Marker, seeking to one, and Transcribe mode's Markers list. Driven through
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

// Adds a Marker at `position` through add_marker, as K does, and returns its id.
song::Id addMarkerAt (MainWindowFixture& f, song::SampleCount position)
{
    run (f, SongIntent { intent::Seek { position } });
    const auto added = runControl (f, SongIntent { intent::AddMarker {} });
    REQUIRE (added.has_value());
    REQUIRE (added->createdId.has_value());
    return *added->createdId;
}

const song::Marker& markerWithId (MainWindowFixture& f, const song::Id& markerId)
{
    const auto* marker = song::findMarker (f.song(), markerId);
    REQUIRE (marker != nullptr);
    return *marker;
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

MarkerFlag& flagOf (MarkerStrip& strip, const song::Id& markerId)
{
    for (auto& flag : strip.flags)
        if (flag->markerId() == markerId)
            return *flag;
    FAIL ("no flag for Marker " << markerId);
    return *strip.flags.front();
}

// The samples per pixel the mode's strip draws at.
song::SampleCount samplesPerPixelOf (MainWindowFixture& f, Mode mode)
{
    auto& view = *f.window->songView();
    return mode == Mode::edit ? view.editPage.timeline.view().samplesPerPixel : view.transcribePage.view().samplesPerPixel;
}

void pressKey (MainWindowFixture& f, int keyCode, juce::juce_wchar character = 0)
{
    f.window->keyPressed (juce::KeyPress (keyCode, {}, character));
}

std::vector<nlohmann::json> eventsNamed (const core::ScopedLogCapture& capture, const std::string& name)
{
    std::vector<nlohmann::json> found;
    for (const auto& event : capture.events())
        if (event.value ("event", "") == name)
            found.push_back (event);
    return found;
}

// Closes the window's Song, which writes song.json, and opens its package again.
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

TEST_CASE ("the Loop/Marker strip sits above the ruler in Transcribe, Edit and Record mode, read-only in Record", "[ui][markers]")
{
    MainWindowFixture f;
    showMode (f, Mode::edit);
    const auto markerId = addMarkerAt (f, 24000);

    SECTION ("Edit: above the timeline's ruler, at the timeline's view")
    {
        auto& timeline = f.window->songView()->editPage.timeline;
        auto& strip = timeline.markerStrip;
        CHECK (isShown (strip));
        CHECK (strip.isEditable());
        CHECK (strip.getBottom() == timeline.rulerBounds().getY());
        CHECK (strip.getX() == timeline.rulerBounds().getX());
        REQUIRE (strip.flags.size() == 1);
        CHECK (flagOf (strip, markerId).getX() == (int) (24000 / timeline.view().samplesPerPixel));
        CHECK (flagOf (strip, markerId).name() == "Marker 1");
    }
    SECTION ("Transcribe: between the overview and the ruler, at the waveform's view")
    {
        showMode (f, Mode::transcribe);
        auto& page = f.window->songView()->transcribePage;
        CHECK (isShown (page.markerStrip));
        CHECK (page.markerStrip.isEditable());
        CHECK (page.markerStrip.getBottom() == page.wave.getY());
        CHECK (page.markerStrip.getX() == page.wave.getX());
        CHECK (page.markerStrip.getY() > page.overview.getBottom());
        CHECK (flagOf (page.markerStrip, markerId).getX() == (int) ((24000 - page.view().firstSample) / page.view().samplesPerPixel));
    }
    SECTION ("Record: read-only, over the whole-Song overview on its scale")
    {
        showMode (f, Mode::record);
        auto& page = f.window->songView()->recordPage;
        CHECK (isShown (page.markerStrip));
        CHECK_FALSE (page.markerStrip.isEditable());
        CHECK (page.markerStrip.getBottom() == page.overview.getY());
        CHECK (page.markerStrip.getX() == page.overview.getX());
        CHECK (page.markerStrip.getWidth() == page.overview.getWidth());
        const auto expectedX = (int) ((double) 24000 * page.overview.getWidth() / (double) page.overview.length());
        CHECK (std::abs (flagOf (page.markerStrip, markerId).getX() - expectedX) <= 1);

        // A drag or double-click on a read-only flag changes nothing.
        auto& flag = flagOf (page.markerStrip, markerId);
        drag (flag, flag.getLocalBounds().getCentre(), flag.getLocalBounds().getCentre() + juce::Point<int> (20, 0));
        doubleClickAt (flag, flag.getLocalBounds().getCentre());
        CHECK (markerWithId (f, markerId).position == 24000);
        CHECK_FALSE (isShown (page.markerStrip.nameField));
    }
}

TEST_CASE ("K adds Marker N at the playhead without stopping playback, one undo step each, saved with the Song", "[ui][markers]")
{
    MainWindowFixture f;
    const auto mode = GENERATE (Mode::edit, Mode::transcribe);
    showMode (f, mode);
    run (f, SongIntent { intent::Seek { 12000 } });
    run (f, SongIntent { intent::PlayOrStop {} });
    f.runBlocks (6);
    REQUIRE (f.openSong->isPlaying());

    const core::ScopedLogCapture capture;
    const auto playhead = f.openSong->position();
    pressKey (f, 'k', 'k');

    REQUIRE (f.song().markers.size() == 1);
    CHECK (f.song().markers[0].name == "Marker 1");
    CHECK (f.song().markers[0].position == playhead);
    CHECK (f.openSong->isPlaying());
    CHECK (f.openSong->package().undoLabel() == song::addMarkerLabel);
    const auto added = eventsNamed (capture, "ui.marker_added");
    REQUIRE (added.size() == 1);
    CHECK (added[0].at ("name") == "Marker 1");
    CHECK (added[0].at ("position") == playhead);
    // The engine plays on without a new plan: a Marker plays nothing.
    CHECK (eventsNamed (capture, "engine.plan_built").empty());

    f.runBlocks (6);
    const auto later = f.openSong->position();
    pressKey (f, 'K', 'K');
    REQUIRE (f.song().markers.size() == 2);
    CHECK (f.song().markers[1].name == "Marker 2");
    CHECK (f.song().markers[1].position == later);
    CHECK (f.openSong->isPlaying());
    run (f, SongIntent { intent::PlayOrStop {} });

    run (f, SongIntent { intent::Undo {} });
    REQUIRE (f.song().markers.size() == 1);
    CHECK (f.song().markers[0].name == "Marker 1");
    run (f, SongIntent { intent::Redo {} });
    REQUIRE (f.song().markers.size() == 2);

    const auto before = f.song().markers;
    reopenSong (f);
    CHECK (f.song().markers == before);
}

TEST_CASE ("Record mode refuses add_marker and the Marker edits with ui.record_mode, and shows why", "[ui][markers][availability]")
{
    MainWindowFixture f;
    showMode (f, Mode::edit);
    const auto markerId = addMarkerAt (f, 1000);
    showMode (f, Mode::record);

    pressKey (f, 'k', 'k');
    CHECK (f.song().markers.size() == 1);
    CHECK (isShown (f.window->noticeLine));
    CHECK (f.window->noticeLine.text() == juce::String ("Make and edit Loops and Markers in Transcribe or Edit mode."));

    const std::vector<Intent> refused {
        SongIntent { intent::AddMarker {} },
        SongIntent { intent::Edit { intent::MoveMarker { markerId, 5000 } } },
        SongIntent { intent::Edit { intent::RenameMarker { markerId, "Solo" } } },
        SongIntent { intent::Edit { intent::DeleteMarker { markerId } } },
    };
    for (const auto& intent : refused)
    {
        const auto result = runControl (f, intent);
        REQUIRE_FALSE (result.has_value());
        CHECK (result.error().code == "ui.record_mode");
        CHECK (result.error().fields.at ("op") == std::string (opName (intent)));
    }
    CHECK (f.song().markers == std::vector<song::Marker> { song::Marker { markerId, "Marker 1", 1000 } });

    // Backspace in Record mode never deletes a Marker selected in Edit mode.
    showMode (f, Mode::edit);
    run (f, SongIntent { intent::SelectMarker { markerId } });
    showMode (f, Mode::record);
    pressKey (f, juce::KeyPress::backspaceKey);
    CHECK (f.song().markers.size() == 1);
}

TEST_CASE ("in Transcribe and Edit, double-clicking a Marker renames it, dragging moves it and Delete removes the selected one, each one undo step", "[ui][markers]")
{
    MainWindowFixture f;
    const auto mode = GENERATE (Mode::edit, Mode::transcribe);
    showMode (f, mode);
    const auto markerId = addMarkerAt (f, 0);
    auto& strip = stripOf (f, mode);
    REQUIRE (strip.isEditable());

    // Rename: the double-click opens the name field over the flag; Return commits what was typed.
    doubleClickAt (flagOf (strip, markerId), flagOf (strip, markerId).getLocalBounds().getCentre());
    REQUIRE (isShown (strip.nameField));
    CHECK (strip.nameField.getText() == "Marker 1");
    strip.nameField.setText ("  Chorus ", juce::sendNotificationSync);
    CHECK (markerWithId (f, markerId).name == "Chorus");
    CHECK (f.openSong->package().undoLabel() == song::renameMarkerLabel);
    CHECK (flagOf (strip, markerId).name() == "Chorus");
    strip.nameField.hideEditor (true);
    CHECK_FALSE (isShown (strip.nameField));

    // Move: 10 pixels right is 10 pixels' worth of samples later.
    const auto perPixel = samplesPerPixelOf (f, mode);
    auto& flag = flagOf (strip, markerId);
    const auto centre = flag.getLocalBounds().getCentre();
    drag (flag, centre, centre + juce::Point<int> (10, 0));
    CHECK (markerWithId (f, markerId).position == 10 * perPixel);
    CHECK (f.openSong->package().undoLabel() == song::moveMarkerLabel);
    CHECK (flagOf (strip, markerId).getX() == stripX (stripScaleFor (mode == Mode::edit ? f.window->songView()->editPage.timeline.view() : f.window->songView()->transcribePage.view()), 10 * perPixel));

    // Delete: a click selects it (and seeks there), then backspace deletes it.
    click (flagOf (strip, markerId));
    CHECK (f.window->songView()->selected (SelectionKind::marker) == markerId);
    CHECK (flagOf (strip, markerId).isSelected());
    CHECK (f.openSong->position() == 10 * perPixel);
    pressKey (f, juce::KeyPress::backspaceKey);
    CHECK (f.song().markers.empty());
    CHECK (f.openSong->package().undoLabel() == song::deleteMarkerLabel);
    CHECK_FALSE (f.window->songView()->selected (SelectionKind::marker).has_value());

    run (f, SongIntent { intent::Undo {} });
    CHECK (markerWithId (f, markerId).name == "Chorus");
    run (f, SongIntent { intent::Undo {} });
    CHECK (markerWithId (f, markerId).position == 0);
    run (f, SongIntent { intent::Undo {} });
    CHECK (markerWithId (f, markerId).name == "Marker 1");
    run (f, SongIntent { intent::Redo {} });
    run (f, SongIntent { intent::Redo {} });
    CHECK (markerWithId (f, markerId).name == "Chorus");
    CHECK (markerWithId (f, markerId).position == 10 * perPixel);
    run (f, SongIntent { intent::Redo {} });
    CHECK (f.song().markers.empty());
    run (f, SongIntent { intent::Undo {} });

    reopenSong (f);
    CHECK (f.song().markers == std::vector<song::Marker> { song::Marker { markerId, "Chorus", 10 * perPixel } });
}

TEST_CASE ("a name of only spaces is refused, and a click on the empty strip clears the selection", "[ui][markers]")
{
    MainWindowFixture f;
    showMode (f, Mode::edit);
    const auto markerId = addMarkerAt (f, 0);

    const auto empty = runControl (f, SongIntent { intent::Edit { intent::RenameMarker { markerId, "   " } } });
    REQUIRE_FALSE (empty.has_value());
    CHECK (empty.error().code == "song.marker_name_empty");
    CHECK (markerWithId (f, markerId).name == "Marker 1");

    auto& strip = stripOf (f, Mode::edit);
    click (flagOf (strip, markerId));
    REQUIRE (f.window->songView()->selected (SelectionKind::marker) == markerId);
    clickAt (strip, { strip.getWidth() - 4, strip.getHeight() / 2 });
    CHECK_FALSE (f.window->songView()->selected (SelectionKind::marker).has_value());

    // Selecting a Clip clears the Marker's selection, and selecting a Marker clears the Clip's.
    click (flagOf (strip, markerId));
    const auto clipId = f.trackNamed ("Band Mix").clips.front().id;
    run (f, SongIntent { intent::SelectClip { clipId } });
    CHECK_FALSE (f.window->songView()->selected (SelectionKind::marker).has_value());
    click (flagOf (strip, markerId));
    CHECK_FALSE (f.window->songView()->selected (SelectionKind::clip).has_value());
    // Backspace now deletes the Marker, not the Clip.
    pressKey (f, juce::KeyPress::backspaceKey);
    CHECK (f.song().markers.empty());
    CHECK (f.trackNamed ("Band Mix").clips.size() == 1);
}

TEST_CASE ("clicking a Marker seeks to it; in Record that moves the record point, so Pre-roll and R work from there", "[ui][markers][record]")
{
    MainWindowFixture f;
    showMode (f, Mode::edit);
    const auto markerId = addMarkerAt (f, 24000);
    run (f, SongIntent { intent::Seek { 0 } });
    showMode (f, Mode::record);
    auto& strip = stripOf (f, Mode::record);

    click (flagOf (strip, markerId));
    CHECK (f.openSong->position() == 24000);
    // Record mode selects nothing: backspace there deletes nothing.
    CHECK_FALSE (f.window->songView()->selected (SelectionKind::marker).has_value());

    SECTION ("R records a Pass from the Marker")
    {
        run (f, SongIntent { intent::Record {} });
        f.runBlocks (8);
        CHECK (f.openSong->runningPassStart() == 24000);
        run (f, SongIntent { intent::PlayOrStop {} });
        f.runBlocks (2);
        REQUIRE (f.song().passes.size() == 1);
        CHECK (f.song().passes.back().startSample == 24000);
    }
    SECTION ("with Pre-roll the Pass waits for the Marker")
    {
        run (f, SongIntent { intent::SetPreRoll { song::PreRoll::oneSecond } });
        run (f, SongIntent { intent::Record {} });
        CHECK (f.openSong->preRollRecordPoint() == 24000);
        run (f, SongIntent { intent::PlayOrStop {} });
        f.runBlocks (2);
    }
}

TEST_CASE ("Transcribe mode's left column lists the Markers in time order, and clicking one seeks to it", "[ui][markers][transcribe]")
{
    MainWindowFixture f;
    showMode (f, Mode::transcribe);
    auto& list = f.window->songView()->transcribePage.markerList;
    CHECK (isShown (list));
    CHECK (isShown (list.heading));
    CHECK (isShown (list.hint));
    CHECK (list.rows.empty());
    CHECK (list.getRight() < f.window->songView()->transcribePage.wave.getX());

    const auto late = addMarkerAt (f, 30000);
    const auto early = addMarkerAt (f, 10000);
    const auto middle = addMarkerAt (f, 20000);
    REQUIRE (list.rows.size() == 3);
    CHECK (list.rows[0]->markerId() == early);
    CHECK (list.rows[1]->markerId() == middle);
    CHECK (list.rows[2]->markerId() == late);
    CHECK_FALSE (isShown (list.hint));
    CHECK (list.rows[0]->getY() < list.rows[1]->getY());

    click (*list.rows[2]);
    CHECK (f.openSong->position() == 30000);
    CHECK (f.window->songView()->selected (SelectionKind::marker) == late);

    // Moving a Marker before the others moves its row to the top.
    run (f, SongIntent { intent::Edit { intent::MoveMarker { late, 0 } } });
    CHECK (list.rows[0]->markerId() == late);
}
