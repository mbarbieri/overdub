#include "ui/MainComponent.h"

#include "ui/MainWindowFixture.h"

#include <catch2/catch_test_macros.hpp>

// Add Track on the Record page (issue #109) through the real window: an empty Track, Armed on the
// device's first input, added as one undo step, that a Pass then records onto.
using namespace overdub::ui::testing;
namespace song = overdub::song;

namespace
{
std::unique_ptr<song::testing::FixtureSong> newSong()
{
    return song::testing::FixtureSongBuilder {}.build();
}
} // namespace

TEST_CASE ("Add Track on a new Song shows one Armed row on the first input", "[ui][record_page][add_track]")
{
    MainWindowFixture f (newSong());
    auto& page = f.window->recordPage;
    REQUIRE (page.rows.empty());
    CHECK (isShown (page.emptyLabel));
    CHECK (isShown (page.addTrackButton));

    click (page.addTrackButton);
    REQUIRE (f.song().tracks.size() == 1);
    CHECK (overdub::song::trackInput (f.song(), f.song().tracks[0].id).armed);
    CHECK (overdub::song::trackInput (f.song(), f.song().tracks[0].id).inputChannels == std::vector<int> { 0 });
    CHECK (f.openSong->package().undoLabel() == "Add Track 1");

    REQUIRE (page.rows.size() == 1);
    auto& row = f.rowNamed ("Track 1");
    CHECK (row.armButton.getToggleState());
    CHECK (row.inputPicker.getText() == "In 1");
    CHECK (! isShown (page.emptyLabel));

    click (page.addTrackButton);
    CHECK (page.rows.size() == 2);
    CHECK (f.openSong->package().undoLabel() == "Add Track 2");
}

TEST_CASE ("a Pass records a Take onto an added Track", "[ui][record_page][add_track]")
{
    MainWindowFixture f (newSong());
    click (f.window->recordPage.addTrackButton);

    REQUIRE (pressKey (*f.window, 'r'));
    f.runBlocks (20);
    CHECK (f.openSong->isRecording());
    REQUIRE (pressKey (*f.window, juce::KeyPress::spaceKey));
    f.runBlocks (1);

    REQUIRE (f.song().passes.size() == 1);
    CHECK (f.openSong->package().undoLabel() == "Record Pass 1");
    CHECK (f.trackNamed ("Track 1").clips.size() == 1);
}

TEST_CASE ("with a device that has no inputs, Add Track adds a Track with no input", "[ui][record_page][add_track]")
{
    MainWindowFixture f (newSong(), {}, {});
    click (f.window->recordPage.addTrackButton);
    REQUIRE (f.song().tracks.size() == 1);
    CHECK (overdub::song::trackInput (f.song(), f.song().tracks[0].id).armed);
    CHECK (overdub::song::trackInput (f.song(), f.song().tracks[0].id).inputChannels.empty());
    CHECK (f.rowNamed ("Track 1").inputPicker.getText() == "No input");
}

TEST_CASE ("undo removes an added Track and redo brings it back Armed", "[ui][record_page][add_track]")
{
    MainWindowFixture f (newSong());
    auto& page = f.window->recordPage;
    click (page.addTrackButton);
    REQUIRE (page.rows.size() == 1);

    click (f.window->topBar.undoButton);
    CHECK (f.song().tracks.empty());
    CHECK (page.rows.empty());

    click (f.window->topBar.redoButton);
    REQUIRE (page.rows.size() == 1);
    CHECK (f.inputOf ("Track 1").armed);
    CHECK (f.rowNamed ("Track 1").armButton.getToggleState());
}

TEST_CASE ("Add Track is disabled while a Pass runs", "[ui][record_page][add_track]")
{
    MainWindowFixture f;
    auto& page = f.window->recordPage;
    CHECK (page.addTrackButton.isEnabled());

    REQUIRE (pressKey (*f.window, 'r'));
    f.runBlocks (10);
    REQUIRE (f.openSong->isRecording());
    CHECK (! page.addTrackButton.isEnabled());
    CHECK (! page.importButton.isEnabled());
    const auto tracksBefore = f.song().tracks.size();
    page.onAddTrack(); // even a direct call adds nothing mid-Pass
    CHECK (f.song().tracks.size() == tracksBefore);

    REQUIRE (pressKey (*f.window, juce::KeyPress::spaceKey));
    f.runBlocks (1);
    CHECK (page.addTrackButton.isEnabled());
}
