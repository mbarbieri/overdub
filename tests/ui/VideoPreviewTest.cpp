#include "ui/VideoPreviewWindow.h"

#include "ui/MainWindowFixture.h"
#include "video/SyntheticMovie.h"

#include <catch2/catch_test_macros.hpp>

// Video Take playback in the window (issue #36), through the real MainComponent over a fixture
// Song with synthetic Video Take movies: the Video toggle and the floating Video Preview, the
// Camera lane's blocks, the frame each screen shows for the playhead, and the Record stage.
using overdub::core::ScopedLogCapture;
using overdub::song::SampleCount;
using namespace overdub::ui;
using namespace overdub::ui::testing;

namespace
{
constexpr SampleCount rate = 48000;

// The fixture Song plus two Video Takes, as one undo step: Pass A's 4 s from 1 s, then Pass B's 2 s
// from 3 s (overlapping A's).
struct VideoWindowFixture : MainWindowFixture
{
    overdub::song::Id olderTake;
    overdub::song::Id newerTake;

    VideoWindowFixture()
    {
        auto changed = song();
        const auto root = openSong->package().location();
        olderTake = overdub::video::testing::addSyntheticVideoTake (changed, root, 1 * rate, 120);
        newerTake = overdub::video::testing::addSyntheticVideoTake (changed, root, 3 * rate, 60);
        REQUIRE (openSong->edit (changed, "Add Video Takes").has_value());
        window->tick();
    }

    void toEditMode()
    {
        REQUIRE (pressKey (*window, '2'));
        REQUIRE (window->mode() == Mode::edit);
    }

    VideoScreen& previewScreen()
    {
        REQUIRE (window->videoPreviewWindow() != nullptr);
        return window->videoPreviewWindow()->screen();
    }
};

int eventCount (const ScopedLogCapture& capture, const std::string& name)
{
    int count = 0;
    for (const auto& event : capture.events())
        if (event.value ("event", "") == name)
            ++count;
    return count;
}
} // namespace

TEST_CASE ("the Video toggle shows and hides the floating Video Preview, which floats in Edit mode only", "[ui][video][preview]")
{
    VideoWindowFixture f;
    ScopedLogCapture capture;
    f.toEditMode();
    auto& toggle = f.window->songView()->editPage.toolbar.videoButton;
    CHECK (isShown (toggle));
    CHECK_FALSE (toggle.getToggleState());
    CHECK_FALSE (f.window->isVideoPreviewShown());

    click (toggle);
    REQUIRE (f.window->videoPreviewWindow() != nullptr);
    CHECK (f.window->isVideoPreviewShown());
    CHECK (f.window->videoPreviewWindow()->getName() == "Video Preview");
    CHECK (toggle.getToggleState());
    CHECK (eventCount (capture, "ui.video_preview_toggled") == 1);

    // Record mode hides it; back in Edit it shows again.
    REQUIRE (pressKey (*f.window, '1'));
    CHECK_FALSE (f.window->isVideoPreviewShown());
    f.toEditMode();
    CHECK (f.window->isVideoPreviewShown());

    click (toggle);
    CHECK_FALSE (f.window->isVideoPreviewShown());
    CHECK_FALSE (toggle.getToggleState());

    // Its close button turns the toggle off too.
    click (toggle);
    REQUIRE (f.window->isVideoPreviewShown());
    f.window->videoPreviewWindow()->closeButtonPressed();
    CHECK_FALSE (f.window->isVideoPreviewShown());
    CHECK_FALSE (f.window->isVideoPreviewWanted());
    CHECK_FALSE (toggle.getToggleState());
}

TEST_CASE ("Camera lane blocks sit at the Video Takes' sample positions and span their movies", "[ui][video][camera_lane]")
{
    VideoWindowFixture f;
    f.toEditMode();
    auto& timeline = f.window->songView()->editPage.timeline;
    timeline.setView (0, 1000); // 1000 samples per pixel: 48 px per second
    const auto laneX = timeline.cameraLaneBounds().getX();

    const auto older = timeline.videoTakeBounds (f.olderTake);
    CHECK (older.getX() == laneX + 48);         // 1 s
    CHECK (older.getRight() == laneX + 5 * 48); // 1 s + 4 s of movie
    const auto newer = timeline.videoTakeBounds (f.newerTake);
    CHECK (newer.getX() == laneX + 3 * 48);
    CHECK (newer.getRight() == laneX + 5 * 48);
    for (const auto& block : { older, newer })
    {
        CHECK (timeline.cameraLaneBounds().contains (block));
        CHECK (block.getHeight() > 0);
    }

    // Zoomed in and scrolled to 2 s: 500 samples per pixel.
    timeline.setView (2 * rate, 500);
    CHECK (timeline.videoTakeBounds (f.newerTake).getX() == laneX + 96);
}

TEST_CASE ("the Video Preview shows the frame at the playhead, the newest Pass's on overlap, and black outside every Video Take", "[ui][video][preview]")
{
    VideoWindowFixture f;
    f.toEditMode();
    click (f.window->songView()->editPage.toolbar.videoButton);

    // Stopped at 2.5 s: Pass A's movie at 1.5 s, frame 45.
    f.openSong->seek (rate * 5 / 2);
    f.runBlocks (1);
    f.window->refreshVideo (f.device.hostTimeNs);
    CHECK (f.previewScreen().shownChoice().videoTakeId == f.olderTake);
    CHECK (f.previewScreen().shownChoice().frameIndex == std::optional<std::size_t> (45));
    CHECK (f.previewScreen().showsVideo());
    CHECK (f.previewScreen().framesDisplayed() == 1);

    // The same frame again is not enqueued again.
    f.window->refreshVideo (f.device.hostTimeNs);
    CHECK (f.previewScreen().framesDisplayed() == 1);

    // Scrubbed to 4 s: both Video Takes cover it, Pass B's (newer) shows its frame 30.
    f.openSong->seek (4 * rate);
    f.runBlocks (1);
    f.window->refreshVideo (f.device.hostTimeNs);
    CHECK (f.previewScreen().shownChoice().videoTakeId == f.newerTake);
    CHECK (f.previewScreen().shownChoice().frameIndex == std::optional<std::size_t> (30));
    CHECK (f.previewScreen().framesDisplayed() == 2);

    // At 6 s nothing covers the playhead.
    f.openSong->seek (6 * rate);
    f.runBlocks (1);
    f.window->refreshVideo (f.device.hostTimeNs);
    CHECK_FALSE (f.previewScreen().showsVideo());
    CHECK (f.previewScreen().message() == noVideoTakeMessage);
}

TEST_CASE ("while playing, the Video Preview shows the frame heard one refresh from now, from the latest anchor", "[ui][video][preview]")
{
    VideoWindowFixture f;
    f.toEditMode();
    click (f.window->songView()->editPage.toolbar.videoButton);
    f.openSong->seek (rate * 3 / 2); // 1.5 s
    f.runBlocks (1);
    REQUIRE (pressKey (*f.window, juce::KeyPress::spaceKey));
    f.runBlocks (100); // about 0.53 s
    REQUIRE (f.openSong->isPlaying());

    const auto anchor = f.engine.latestAnchor();
    REQUIRE (anchor.has_value());
    // No output latency in the fixture binding: heard = anchor + 10 ms + one 60 Hz refresh (800).
    f.window->refreshVideo (anchor->hostTimeNs + 10'000'000);
    const auto heard = anchor->songSample + 480 + 800;
    const auto expectedFrame = (std::size_t) ((heard - rate) * 30 / rate); // Pass A from 1 s, 30 fps
    CHECK (f.previewScreen().shownChoice().videoTakeId == f.olderTake);
    CHECK (f.previewScreen().shownChoice().frameIndex == std::optional<std::size_t> (expectedFrame));
    REQUIRE (pressKey (*f.window, juce::KeyPress::spaceKey));
}

TEST_CASE ("the Record stage shows playback when not recording, and the camera's feed while Armed and stopped or recording", "[ui][video][record_page]")
{
    VideoWindowFixture f;
    auto& page = f.window->songView()->recordPage;
    f.openSong->seek (2 * rate);
    f.runBlocks (1);

    // Disarmed: playback, Pass A's frame 30 at 2 s.
    CHECK (page.showsStagePlayback());
    f.window->refreshVideo (f.device.hostTimeNs);
    CHECK (page.stageVideo.shownChoice().videoTakeId == f.olderTake);
    CHECK (page.stageVideo.shownChoice().frameIndex == std::optional<std::size_t> (30));

    // Armed and stopped: the live feed, to frame the shot.
    click (page.cameraArmButton);
    REQUIRE (f.song().cameraArmed);
    CHECK_FALSE (page.showsStagePlayback());
    CHECK (isShown (page.cameraStage));

    // Armed and playing: playback.
    REQUIRE (pressKey (*f.window, juce::KeyPress::spaceKey));
    f.runBlocks (1);
    CHECK (page.showsStagePlayback());
    REQUIRE (pressKey (*f.window, juce::KeyPress::spaceKey));
    f.runBlocks (1);

    // Recording with the camera Armed: the live feed.
    f.camera.movie.frames = 10;
    REQUIRE (pressKey (*f.window, 'r'));
    f.runBlocks (20);
    REQUIRE (f.openSong->isRecording());
    CHECK_FALSE (page.showsStagePlayback());
    REQUIRE (pressKey (*f.window, juce::KeyPress::spaceKey));
    f.runBlocks (1);
}

TEST_CASE ("which the Record stage shows follows the camera arm, the Pass and the transport", "[ui][video][record_page]")
{
    CHECK_FALSE (stageShowsLiveFeed (false, false, false));
    CHECK_FALSE (stageShowsLiveFeed (false, true, true));
    CHECK_FALSE (stageShowsLiveFeed (false, false, true));
    CHECK (stageShowsLiveFeed (true, false, false));
    CHECK (stageShowsLiveFeed (true, true, true));
    CHECK_FALSE (stageShowsLiveFeed (true, false, true));
}
