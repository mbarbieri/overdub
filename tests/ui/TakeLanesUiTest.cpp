#include "ui/Timeline.h"

#include "song/ClipEdits.h"
#include "song/TakeEdits.h"
#include "ui/MainWindowFixture.h"

#include <catch2/catch_test_macros.hpp>

// Take Lanes and "Passes ▾" through the real window (issue #30, ADR 0005 UI amendment): unfold a
// Track's lanes, click a lane, open the Passes panel and bring a Pass back; then the Song, the
// undo step's label, and what one undo restores.
using overdub::song::SampleCount;
using namespace overdub::ui;
using namespace overdub::ui::testing;
namespace song = overdub::song;

namespace
{
// Two Passes recorded on Guitar and Vocals: Pass 1 from 0 for about 3 s, Pass 2 from 1 s for about
// 1 s on top. Then Edit mode.
struct TakeLanesFixture : MainWindowFixture
{
    TakeLanesFixture()
    {
        recordPass (0, 560);
        recordPass (48000, 190);
        REQUIRE (song().passes.size() == 2);
        REQUIRE (trackNamed ("Guitar").clips.size() == 2);
        REQUIRE (pressKey (*window, '2'));
        REQUIRE (isShown (timeline()));
    }

    void recordPass (SampleCount from, int blocks)
    {
        openSong->seek (from);
        runBlocks (0);
        REQUIRE (pressKey (*window, 'r'));
        runBlocks (blocks);
        REQUIRE (pressKey (*window, juce::KeyPress::spaceKey));
        runBlocks (1);
    }

    Timeline& timeline() { return window->songView()->editPage.timeline; }

    TrackHeader& headerOf (const std::string& trackName)
    {
        const auto& id = trackNamed (trackName).id;
        for (auto& header : timeline().headers)
            if (header->trackId() == id)
                return *header;
        FAIL ("no header for Track " << trackName);
        return *timeline().headers.front();
    }

    // The Take of Pass `number` recorded on this Track.
    const song::Take& takeOf (const std::string& trackName, int number)
    {
        const auto& passId = song().passes[(std::size_t) number - 1].id;
        const auto& trackId = trackNamed (trackName).id;
        for (const auto& take : song().takes)
            if (take.passId == passId && take.trackId == trackId)
                return take;
        FAIL ("no Take of Pass " << number << " on " << trackName);
        return song().takes.front();
    }

    // A point on this Take's lane at Song sample `at`.
    juce::Point<int> lanePoint (const song::Id& takeId, SampleCount at)
    {
        const auto lane = timeline().takeLaneBounds (takeId);
        REQUIRE (! lane.isEmpty());
        const auto x = xForSample (timeline().view(), at);
        REQUIRE (lane.getX() <= x);
        REQUIRE (x < lane.getRight());
        return { x, lane.getCentreY() };
    }

    void undoRestores (const song::Song& before)
    {
        REQUIRE (pressKey (*window, 'z', juce::ModifierKeys::commandModifier));
        CHECK (song() == before);
    }
};
} // namespace

TEST_CASE ("a Track header's Takes toggle unfolds one lane per Take, newest Pass first, without changing the Song", "[ui][take_lanes]")
{
    TakeLanesFixture f;
    const auto before = f.song();
    auto& guitar = f.headerOf ("Guitar");
    CHECK (isShown (guitar.takesButton));
    CHECK (guitar.takesButton.getButtonText() == juce::String::fromUTF8 ("\xE2\x96\xB8 2 Takes"));
    CHECK (! isShown (f.headerOf ("Band Mix").takesButton)); // no Takes recorded there
    CHECK (f.timeline().takeLaneBounds (f.takeOf ("Guitar", 1).id).isEmpty());

    const auto vocalsTop = f.timeline().clipBounds (f.trackNamed ("Vocals").clips[0].id).getY();
    click (guitar.takesButton);

    REQUIRE (f.timeline().areTakeLanesUnfolded (f.trackNamed ("Guitar").id));
    CHECK (guitar.takesButton.getButtonText() == juce::String::fromUTF8 ("\xE2\x96\xBE 2 Takes"));
    const auto lanes = f.timeline().takeLanesShown (f.trackNamed ("Guitar").id);
    REQUIRE (lanes.size() == 2);
    CHECK (lanes[0].takeId == f.takeOf ("Guitar", 2).id);
    CHECK (lanes[1].takeId == f.takeOf ("Guitar", 1).id);
    CHECK (lanes[0].status == song::HeardStatus::heard);
    CHECK (lanes[1].status == song::HeardStatus::partlyHeard);
    const auto pass2Lane = f.timeline().takeLaneBounds (lanes[0].takeId);
    const auto pass1Lane = f.timeline().takeLaneBounds (lanes[1].takeId);
    CHECK (pass2Lane.getY() >= f.timeline().clipBounds (f.trackNamed ("Guitar").clips[0].id).getBottom());
    CHECK (pass1Lane.getY() == pass2Lane.getBottom());
    // The next Track moves down to make room.
    CHECK (f.timeline().clipBounds (f.trackNamed ("Vocals").clips[0].id).getY() == vocalsTop + 2 * takeLaneHeight);

    CHECK (f.song() == before);
    CHECK (f.openSong->package().undoLabel() == "Record Pass 2");

    click (guitar.takesButton);
    CHECK (! f.timeline().areTakeLanesUnfolded (f.trackNamed ("Guitar").id));
    CHECK (f.timeline().takeLaneBounds (lanes[0].takeId).isEmpty());
}

TEST_CASE ("clicking an older Take's lane where a newer Clip sounds swaps that Clip, as one Use Pass step", "[ui][take_lanes]")
{
    TakeLanesFixture f;
    f.timeline().setTakeLanesUnfolded (f.trackNamed ("Guitar").id, true);
    const auto before = f.song();
    const auto topClip = f.trackNamed ("Guitar").clips[1];
    const auto pass1 = f.takeOf ("Guitar", 1);

    clickAt (f.timeline(), f.lanePoint (pass1.id, topClip.timelineStart + topClip.length / 2));

    const auto& clips = f.trackNamed ("Guitar").clips;
    REQUIRE (clips.size() == 2);
    CHECK (clips[1].id == topClip.id); // same Clip, same layer
    CHECK (clips[1].source.id == pass1.id);
    CHECK (clips[1].sourceOffset == clips[1].timelineStart - pass1.origin);
    CHECK (clips[0] == before.tracks[1].clips[0]);
    CHECK (f.openSong->package().undoLabel() == "Use Pass 1 on Guitar");
    CHECK (f.window->topBar.undoButton.getButtonText() == "Undo Use Pass 1 on Guitar");
    CHECK (f.window->songView()->selected (SelectionKind::clip) == topClip.id);
    // The lanes redraw: Pass 2 is no longer heard.
    CHECK (f.timeline().takeLanesShown (f.trackNamed ("Guitar").id)[0].status == song::HeardStatus::notHeard);
    f.undoRestores (before);
}

TEST_CASE ("clicking a lane where the Take already sounds, or outside its audio, changes nothing", "[ui][take_lanes]")
{
    TakeLanesFixture f;
    f.timeline().setTakeLanesUnfolded (f.trackNamed ("Guitar").id, true);
    const auto before = f.song();
    const auto pass1 = f.takeOf ("Guitar", 1);

    clickAt (f.timeline(), f.lanePoint (pass1.id, 1000)); // Pass 1 is heard there
    CHECK (f.song() == before);
    CHECK (f.openSong->package().undoLabel() == "Record Pass 2");
}

TEST_CASE ("clicking a lane where no Take Clip sounds brings the whole Take back on top, as one step", "[ui][take_lanes]")
{
    TakeLanesFixture f;
    const auto pass1 = f.takeOf ("Guitar", 1);
    REQUIRE (f.openSong->edit (*song::deleteClip (f.song(), f.trackNamed ("Guitar").clips[0].id), "Delete Clip").has_value());
    f.window->tick();
    f.timeline().setTakeLanesUnfolded (f.trackNamed ("Guitar").id, true);
    const auto before = f.song();

    clickAt (f.timeline(), f.lanePoint (pass1.id, 1000)); // a gap now

    const auto& clips = f.trackNamed ("Guitar").clips;
    REQUIRE (clips.size() == 2);
    const auto expected = song::clipAsRecorded (pass1, f.song().passes[0].startSample, clips[1].id);
    REQUIRE (expected.has_value());
    CHECK (clips[1] == *expected);
    CHECK (f.openSong->package().undoLabel() == "Bring back Pass 1 on Guitar");
    CHECK (f.window->songView()->selected (SelectionKind::clip) == clips[1].id);
    f.undoRestores (before);
}

TEST_CASE ("Passes opens the Passes list, and Bring back puts the whole Pass back on top as one step", "[ui][take_lanes]")
{
    TakeLanesFixture f;
    auto& page = f.window->songView()->editPage;
    const auto before = f.song();
    CHECK (! page.isPassesPanelOpen());

    click (page.passesButton);
    REQUIRE (page.isPassesPanelOpen());
    REQUIRE (isShown (page.passesPanel));
    REQUIRE (page.passesPanel.numRows() == 2);
    auto& newest = *page.passesPanel.rowComponents[0];
    auto& oldest = *page.passesPanel.rowComponents[1];
    CHECK (newest.passId == f.song().passes[1].id);
    CHECK (newest.status.getText() == "heard");
    CHECK (oldest.title.getText().startsWith ("Pass 1"));
    CHECK (oldest.status.getText() == "partly heard");
    CHECK (! page.passesPanel.getBounds().intersects (page.passesButton.getBounds()));

    click (oldest.bringBackButton);

    CHECK (! page.isPassesPanelOpen());
    for (const auto* trackName : { "Guitar", "Vocals" })
    {
        const auto& clips = f.trackNamed (trackName).clips;
        REQUIRE (clips.size() == 3);
        CHECK (clips[2].source.id == f.takeOf (trackName, 1).id);
        CHECK (clips[2].timelineStart == f.song().passes[0].startSample);
    }
    CHECK (f.trackNamed ("Band Mix").clips == before.tracks[0].clips);
    CHECK (song::passHeardStatus (f.song(), f.song().passes[0].id) == song::HeardStatus::heard);
    CHECK (f.openSong->package().undoLabel() == "Bring back Pass 1");
    f.undoRestores (before);
}

TEST_CASE ("Take Lanes and Bring back do nothing while a Pass records", "[ui][take_lanes][record]")
{
    TakeLanesFixture f;
    auto& page = f.window->songView()->editPage;
    f.timeline().setTakeLanesUnfolded (f.trackNamed ("Guitar").id, true);
    click (page.passesButton);
    const auto pass1 = f.takeOf ("Guitar", 1);
    const auto topClip = f.trackNamed ("Guitar").clips[1];
    const auto point = f.lanePoint (pass1.id, topClip.timelineStart + 100);

    f.openSong->seek (500000);
    f.runBlocks (0);
    REQUIRE (pressKey (*f.window, 'r'));
    f.runBlocks (10);
    REQUIRE (f.openSong->isRecording());
    const auto before = f.song();

    clickAt (f.timeline(), point);
    CHECK (! page.passesPanel.rowComponents[1]->bringBackButton.isEnabled());
    CHECK (f.song() == before);

    REQUIRE (pressKey (*f.window, juce::KeyPress::spaceKey));
    f.runBlocks (1);
    CHECK (f.openSong->package().undoLabel() == "Record Pass 3");
}
