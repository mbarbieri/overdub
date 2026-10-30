#include "ui/EditPage.h"

#include "song/VideoTakeEdits.h"
#include "ui/MainWindowFixture.h"
#include "video/SyntheticMovie.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <algorithm>
#include <vector>

// Selecting and deleting a Video Take on the Edit Timeline (issue #128) through the real window:
// clicks in the Camera lane, the Clip toolbar, the delete keys and button, then the Song, the undo
// step, the package's movie files, the Export choice and the Video Preview.
using overdub::song::SampleCount;
using namespace overdub::ui;
using namespace overdub::ui::testing;

namespace
{
constexpr SampleCount rate = 48000;

// The fixture Song plus two Video Takes, as one undo step: Pass A's 4 s from 1 s, then Pass B's 2 s
// from 3 s (overlapping A's). In Edit mode, at 1000 samples per pixel.
struct VideoTakeFixture : MainWindowFixture
{
    overdub::song::Id olderTake;
    overdub::song::Id newerTake;

    VideoTakeFixture()
    {
        auto changed = song();
        const auto root = openSong->package().location();
        olderTake = overdub::video::testing::addSyntheticVideoTake (changed, root, 1 * rate, 120);
        newerTake = overdub::video::testing::addSyntheticVideoTake (changed, root, 3 * rate, 60);
        REQUIRE (openSong->edit (changed, "Add Video Takes").has_value());
        window->tick();
        REQUIRE (pressKey (*window, '2'));
        timeline().setView (0, 1000);
    }

    Timeline& timeline() { return window->songView()->editPage.timeline; }
    ClipToolbar& toolbar() { return window->songView()->editPage.toolbar; }

    // A point inside the older Video Take's block that the newer one does not cover (2 s).
    juce::Point<int> olderTakePoint()
    {
        const auto block = timeline().videoTakeBounds (olderTake);
        REQUIRE (! block.isEmpty());
        return { block.getX() + 48, block.getCentreY() };
    }

    void selectOlderTake()
    {
        clickAt (timeline(), olderTakePoint());
        REQUIRE (timeline().selectedVideoTake() == olderTake);
    }

    juce::Rectangle<int> bandMixBounds()
    {
        const auto bounds = timeline().clipBounds (trackNamed ("Band Mix").clips.at (0).id);
        REQUIRE (! bounds.isEmpty());
        return bounds;
    }

    juce::File movieOf (const overdub::song::Id& videoTakeId)
    {
        return openSong->package().location().getChildFile ("video/" + videoTakeId + ".mov");
    }
};

bool hasVideoTake (const overdub::song::Song& song, const overdub::song::Id& videoTakeId)
{
    return overdub::song::findVideoTake (song, videoTakeId) != nullptr;
}
} // namespace

TEST_CASE ("the toolbar for a selected Video Take names its Pass, start and length, and only Delete is enabled", "[ui][edit][video_takes]")
{
    auto song = overdub::song::makeEmptySong (48000);
    song.videoTakes.push_back ({ .id = "v", .passId = "p" });
    const std::vector<overdub::video::VideoTakeSpan> spans { { .videoTakeId = "v", .title = "Pass 2", .start = 48000, .end = 48000 * 5 } };

    const auto state = videoTakeToolbarStateFor (song, spans, "v", true);
    CHECK (state.hasSelection);
    CHECK (state.details == "Video Take \xC2\xB7 Pass 2 \xC2\xB7 start 1.000 s \xC2\xB7 length 4.000 s");
    CHECK (state.canDelete);
    CHECK (! state.canSplit);
    CHECK (! state.canEditFadesAndGain);
    CHECK (state.fadeInText.empty());
    CHECK (state.gainText.empty());

    CHECK (! videoTakeToolbarStateFor (song, spans, "v", false).canDelete);
    CHECK (! videoTakeToolbarStateFor (song, spans, "gone", true).hasSelection);

    // A Video Take whose movie cannot be read has no known length.
    const std::vector<overdub::video::VideoTakeSpan> unreadable { { .videoTakeId = "v", .title = "Pass 2", .start = 48000, .end = 48000, .readable = false } };
    CHECK (videoTakeToolbarStateFor (song, unreadable, "v", true).details == "Video Take \xC2\xB7 Pass 2 \xC2\xB7 start 1.000 s \xC2\xB7 movie missing");
}

TEST_CASE ("clicking a Video Take selects it; selecting a Clip or empty lane space clears it", "[ui][edit][video_takes]")
{
    VideoTakeFixture f;
    const auto before = f.song();

    // A Clip, then the Video Take: the Clip's selection goes.
    clickAt (f.timeline(), f.bandMixBounds().getCentre());
    REQUIRE (f.timeline().selectedClip().has_value());
    clickAt (f.timeline(), f.olderTakePoint());
    CHECK (f.timeline().selectedVideoTake() == f.olderTake);
    CHECK (! f.timeline().selectedClip().has_value());
    CHECK (f.song() == before);

    // Where the blocks overlap, the newer one (drawn on top) is picked.
    clickAt (f.timeline(), f.timeline().videoTakeBounds (f.newerTake).getCentre());
    CHECK (f.timeline().selectedVideoTake() == f.newerTake);

    // A Clip clears the Video Take.
    clickAt (f.timeline(), f.bandMixBounds().getCentre());
    CHECK (f.timeline().selectedClip().has_value());
    CHECK (! f.timeline().selectedVideoTake().has_value());

    // Empty Camera lane space clears both.
    f.selectOlderTake();
    const auto lane = f.timeline().cameraLaneBounds();
    clickAt (f.timeline(), { lane.getX() + 10, lane.getCentreY() }); // before 1 s: no block
    CHECK (! f.timeline().selectedVideoTake().has_value());
    CHECK (! f.timeline().selectedClip().has_value());

    // Empty Track lane space clears it too.
    f.selectOlderTake();
    clickAt (f.timeline(), { f.bandMixBounds().getRight() + 300, f.bandMixBounds().getCentreY() });
    CHECK (! f.timeline().selectedVideoTake().has_value());
    CHECK (f.song() == before);
}

TEST_CASE ("with a Video Take selected the toolbar shows it with Delete enabled, and S does nothing", "[ui][edit][video_takes]")
{
    VideoTakeFixture f;
    f.openSong->seek (2 * rate); // inside the Video Take and the Band Mix Clip
    f.runBlocks (0);
    f.selectOlderTake();
    const auto before = f.song();

    CHECK (f.toolbar().details.getText() == juce::String::fromUTF8 ("Video Take \xC2\xB7 Pass 1 \xC2\xB7 start 1.000 s \xC2\xB7 length 4.000 s"));
    CHECK (f.toolbar().deleteButton.isEnabled());
    CHECK (! f.toolbar().splitButton.isEnabled());
    CHECK (! f.toolbar().fadeInField.isEnabled());
    CHECK (! f.toolbar().fadeOutField.isEnabled());
    CHECK (! f.toolbar().gainField.isEnabled());

    REQUIRE (pressKey (*f.window, 's'));
    CHECK (f.song() == before);
    CHECK (f.openSong->package().undoLabel() == "Add Video Takes");
}

TEST_CASE ("backspace, forward delete and the Delete button each delete the selected Video Take as one undo step", "[ui][edit][video_takes]")
{
    const auto deleteWith = GENERATE (0, 1, 2);
    VideoTakeFixture f;
    f.selectOlderTake();
    const auto before = f.song();

    if (deleteWith == 0)
        REQUIRE (pressKey (*f.window, juce::KeyPress (juce::KeyPress::backspaceKey)));
    else if (deleteWith == 1)
        REQUIRE (pressKey (*f.window, juce::KeyPress (juce::KeyPress::deleteKey)));
    else
        click (f.toolbar().deleteButton);

    CHECK (! hasVideoTake (f.song(), f.olderTake));
    CHECK (hasVideoTake (f.song(), f.newerTake));
    CHECK (f.song().passes == before.passes);
    CHECK (f.song().takes == before.takes);
    CHECK (f.openSong->package().undoLabel() == "Delete Video Take");
    CHECK (! f.timeline().selectedVideoTake().has_value());
    CHECK (f.timeline().videoTakeBounds (f.olderTake).isEmpty());
    CHECK (f.movieOf (f.olderTake).existsAsFile()); // media is never deleted

    REQUIRE (pressKey (*f.window, 'z', juce::ModifierKeys::commandModifier));
    CHECK (f.song() == before);
    CHECK (! f.timeline().videoTakeBounds (f.olderTake).isEmpty());

    REQUIRE (pressKey (*f.window, 'z', juce::ModifierKeys::commandModifier | juce::ModifierKeys::shiftModifier));
    CHECK (! hasVideoTake (f.song(), f.olderTake));
    CHECK (f.movieOf (f.olderTake).existsAsFile());
}

TEST_CASE ("deleting the Export's chosen Video Take puts the choice back to the default", "[ui][edit][video_takes][export]")
{
    VideoTakeFixture f;
    REQUIRE (f.window->run (SongIntent { intent::SetExportVideoTake { f.olderTake } }, Origin::control).has_value());
    REQUIRE (f.song().settings.exportVideoTakeId == f.olderTake);

    f.selectOlderTake();
    REQUIRE (pressKey (*f.window, juce::KeyPress (juce::KeyPress::backspaceKey)));
    CHECK (! f.song().settings.exportVideoTakeId.has_value());

    // The choice is outside undo history: undo brings the Video Take back, the choice stays default.
    REQUIRE (pressKey (*f.window, 'z', juce::ModifierKeys::commandModifier));
    CHECK (overdub::song::findVideoTake (f.song(), f.olderTake) != nullptr);
    CHECK (! f.song().settings.exportVideoTakeId.has_value());
}

TEST_CASE ("the Video Preview stops showing a deleted Video Take", "[ui][edit][video_takes][preview]")
{
    VideoTakeFixture f;
    // The Video Preview shows playback while the camera is Armed and the transport plays.
    REQUIRE (f.window->run (SongIntent { intent::SetCameraArmed { true } }, Origin::control).has_value());
    auto& preview = f.window->songView()->editPage.videoPreview;
    f.openSong->seek (2 * rate);
    f.runBlocks (1);
    REQUIRE (pressKey (*f.window, juce::KeyPress::spaceKey));
    f.runBlocks (1);
    REQUIRE (preview.showsPlayback());
    f.window->refreshVideo (f.device.hostTimeNs);
    REQUIRE (preview.playback.shownChoice().videoTakeId == f.olderTake);

    f.selectOlderTake();
    REQUIRE (pressKey (*f.window, juce::KeyPress (juce::KeyPress::backspaceKey)));
    f.window->refreshVideo (f.device.hostTimeNs);
    CHECK_FALSE (preview.playback.showsVideo());
    REQUIRE (pressKey (*f.window, juce::KeyPress::spaceKey));
    f.runBlocks (1);
}

TEST_CASE ("a Video Take cannot be deleted while a Pass runs", "[ui][edit][video_takes][record]")
{
    VideoTakeFixture f;
    f.selectOlderTake();
    REQUIRE (pressKey (*f.window, 'r'));
    f.runBlocks (10);
    REQUIRE (f.openSong->isRecording());
    const auto videoTakesBefore = f.song().videoTakes;

    CHECK (! f.toolbar().deleteButton.isEnabled());
    REQUIRE (pressKey (*f.window, juce::KeyPress (juce::KeyPress::backspaceKey)));
    REQUIRE (pressKey (*f.window, juce::KeyPress (juce::KeyPress::deleteKey)));
    CHECK (f.song().videoTakes == videoTakesBefore);

    REQUIRE (pressKey (*f.window, juce::KeyPress::spaceKey));
    f.runBlocks (1);
    CHECK (f.openSong->package().undoLabel() == "Record Pass 3");
}
