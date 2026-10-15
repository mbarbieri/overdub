#include "ui/MainComponent.h"

#include "core/LogEvents.h"
#include "export/MixRender.h"
#include "song/FakeSeparationHelper.h"
#include "ui/MainWindowFixture.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <memory>
#include <string>
#include <vector>

// Deleting a Track from the Edit page (issue #124) through the real window: the × in a Track's
// header, the injected confirmation, then the Song, the undo step and what the window shows.
using namespace overdub;
using namespace overdub::ui;
using namespace overdub::ui::testing;

namespace
{
// Clicks the Track's × and answers the confirmation.
void deleteTrackNamed (MainWindowFixture& f, const std::string& name, bool confirmed = true)
{
    click (f.editHeaderNamed (name).deleteButton);
    f.answerDeleteQuestion (confirmed);
}

std::vector<std::string> trackNames (const song::Song& song)
{
    std::vector<std::string> names;
    for (const auto& track : song.tracks)
        names.push_back (track.name);
    return names;
}

std::vector<song::Id> headerTrackIds (MainWindowFixture& f)
{
    std::vector<song::Id> ids;
    for (const auto& header : f.window->editPage.timeline.headers)
        ids.push_back (header->trackId());
    return ids;
}

std::vector<song::Id> stripTrackIds (MainWindowFixture& f)
{
    std::vector<song::Id> ids;
    for (const auto& strip : f.window->editPage.mixer.strips)
        ids.push_back (strip->trackId());
    return ids;
}

std::vector<song::Id> trackIds (const song::Song& song)
{
    std::vector<song::Id> ids;
    for (const auto& track : song.tracks)
        ids.push_back (track.id);
    return ids;
}
} // namespace

TEST_CASE ("confirming the × on a Track's Edit header deletes it as one labelled undo step", "[ui][delete_track]")
{
    MainWindowFixture f;
    REQUIRE (pressKey (*f.window, '2'));
    const auto takesBefore = f.song().takes;
    const auto backingTracksBefore = f.song().backingTracks;

    click (f.editHeaderNamed ("Guitar").deleteButton);
    REQUIRE (f.deleteQuestionsAsked.size() == 1);
    CHECK (f.song().tracks.size() == 3); // nothing changes before the answer
    const auto guitarId = f.trackNamed ("Guitar").id;
    core::ScopedLogCapture capture;
    f.answerDeleteQuestion (true);

    CHECK (f.deleteQuestionsAsked[0] == juce::String ("Delete Track \"Guitar\"? Its Clips and effects will be removed. You can undo this."));
    CHECK (trackNames (f.song()) == std::vector<std::string> { "Band Mix", "Vocals" });
    CHECK (f.song().takes == takesBefore);
    CHECK (f.song().backingTracks == backingTracksBefore);
    CHECK (headerTrackIds (f) == trackIds (f.song()));
    CHECK (stripTrackIds (f) == trackIds (f.song()));
    CHECK (f.openSong->package().undoLabel() == "Delete Track Guitar");
    const auto deleted = core::testing::firstEvent (capture, "ui.track_deleted");
    CHECK (deleted.at ("track_id") == guitarId);
    CHECK (deleted.at ("label") == "Delete Track Guitar");
}

TEST_CASE ("cancelling the confirmation leaves the Song and its history unchanged", "[ui][delete_track]")
{
    MainWindowFixture f;
    REQUIRE (pressKey (*f.window, '2'));
    const auto songBefore = f.song();
    const auto labelBefore = f.openSong->package().undoLabel();

    deleteTrackNamed (f, "Guitar", false);

    CHECK (f.deleteQuestionsAsked.size() == 1);
    CHECK (f.song() == songBefore);
    CHECK (f.openSong->package().undoLabel() == labelBefore);
}

TEST_CASE ("undo brings a deleted Track back where it was, and redo deletes it again", "[ui][delete_track]")
{
    MainWindowFixture f;
    REQUIRE (pressKey (*f.window, '2'));
    auto mixed = f.song();
    auto& bandMix = mixed.tracks.at (0);
    bandMix.volumeDb = -6.0;
    bandMix.pan = 0.5;
    bandMix.insertChain.push_back (song::InsertSlot { .id = "slot", .component = { "aufx", "dely", "appl" }, .name = "AUDelay" });
    REQUIRE (f.openSong->edit (mixed, "Set up Band Mix").has_value());
    const auto songBefore = f.song();
    REQUIRE (songBefore.tracks.at (0).insertChain.size() == 1);
    deleteTrackNamed (f, "Band Mix");
    REQUIRE (trackNames (f.song()) == std::vector<std::string> { "Guitar", "Vocals" });

    REQUIRE (pressKey (*f.window, 'z', juce::ModifierKeys::commandModifier));
    CHECK (f.song() == songBefore);
    CHECK (headerTrackIds (f) == trackIds (songBefore));

    REQUIRE (pressKey (*f.window, 'z', juce::ModifierKeys::commandModifier | juce::ModifierKeys::shiftModifier));
    CHECK (trackNames (f.song()) == std::vector<std::string> { "Guitar", "Vocals" });
}

TEST_CASE ("an Armed Track and the last Track can be deleted, leaving a Song with no Tracks", "[ui][delete_track]")
{
    MainWindowFixture f { song::testing::FixtureSongBuilder {}.track ("Guitar", true, { 0 }).build() };
    REQUIRE (pressKey (*f.window, '2'));
    REQUIRE (f.trackNamed ("Guitar").armed);

    deleteTrackNamed (f, "Guitar");

    CHECK (f.song().tracks.empty());
    CHECK (f.window->editPage.timeline.headers.empty());
}

TEST_CASE ("deleting a Track clears a selection on it and stops its Monitoring", "[ui][delete_track]")
{
    MainWindowFixture f;
    REQUIRE (pressKey (*f.window, '2'));
    const auto bandMixClip = f.trackNamed ("Band Mix").clips.at (0).id;
    f.window->editPage.timeline.selectClip (bandMixClip);
    const auto guitarId = f.trackNamed ("Guitar").id;
    f.openSong->setMonitoring (guitarId, true);
    REQUIRE (f.openSong->isMonitoring (guitarId));

    deleteTrackNamed (f, "Band Mix");
    deleteTrackNamed (f, "Guitar");

    CHECK_FALSE (f.window->editPage.timeline.selectedClip().has_value());
    CHECK_FALSE (f.openSong->isMonitoring (guitarId));
    REQUIRE (pressKey (*f.window, 'z', juce::ModifierKeys::commandModifier));
    CHECK_FALSE (f.openSong->isMonitoring (guitarId));
}

TEST_CASE ("the × is disabled while a Pass runs", "[ui][delete_track]")
{
    MainWindowFixture f;
    REQUIRE (pressKey (*f.window, '2'));
    REQUIRE (pressKey (*f.window, 'r'));
    REQUIRE (f.openSong->isRecording());

    CHECK_FALSE (f.editHeaderNamed ("Guitar").deleteButton.isEnabled());
}

TEST_CASE ("the × is disabled while an Export runs", "[ui][delete_track]")
{
    MainWindowFixture f;
    REQUIRE (pressKey (*f.window, '2'));
    REQUIRE (f.openSong->startExport ({ exporting::ExportFormat::wav, f.fixture->folder.getChildFile ("Song.wav") }).has_value());
    f.window->tick();

    CHECK_FALSE (f.editHeaderNamed ("Guitar").deleteButton.isEnabled());
}

TEST_CASE ("the × is disabled while a Separation runs", "[ui][delete_track]")
{
    MainWindowFixture f { song::testing::FixtureSongBuilder {}.sampleRate (song::testing::helperSampleRate).backingTrack ("Band Mix", 2.0, 220.0).build() };
    REQUIRE (pressKey (*f.window, '2'));
    f.openSong->setSeparationTools (song::SeparationTools {
        .model = song::testing::writeStandInModel (f.fixture->folder),
        .startHelper = song::testing::fakeSeparationHelper ({}, std::make_shared<song::testing::FakeHelperLog>()),
        .workFolder = f.fixture->folder.getChildFile ("work"),
    });
    REQUIRE (f.openSong->startSeparation (f.trackNamed ("Band Mix").id).has_value());
    f.window->refresh();

    CHECK_FALSE (f.editHeaderNamed ("Band Mix").deleteButton.isEnabled());
}

TEST_CASE ("snapshot: the Edit page's Track headers with their ×", "[ui][delete_track][snapshot]")
{
    MainWindowFixture f;
    REQUIRE (pressKey (*f.window, '2'));
    CHECK (writeSnapshot (*f.window, "issue-124", "edit-track-headers").existsAsFile());
}
