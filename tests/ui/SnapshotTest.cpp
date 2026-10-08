#include "ui/MainWindowFixture.h"
#include "video/SyntheticMovie.h"
#include "ui/MixerGeometry.h"

#include <catch2/catch_test_macros.hpp>

#include <cmath>

using namespace overdub::ui::testing;

// Renders each surface of this slice for a session to look at (ADR 0005 UI amendment):
// build/ui-snapshots/slice-06/*.png. Nothing here asserts on pixels.
TEST_CASE ("snapshots of Record mode, idle and recording, and the Export placeholder", "[ui][snapshot]")
{
    const std::string slice = "slice-06";
    MainWindowFixture f;
    auto& window = *f.window;
    window.tick();

    CHECK (writeSnapshot (window, slice, "record-idle").existsAsFile());

    REQUIRE (pressKey (window, 'r'));
    f.runBlocks (400); // about 2.1 s at 48 kHz
    REQUIRE (f.openSong->isRecording());
    CHECK (writeSnapshot (window, slice, "record-recording").existsAsFile());
    REQUIRE (pressKey (window, juce::KeyPress::spaceKey));
    f.runBlocks (1);
    REQUIRE (! f.openSong->isRecording());

    REQUIRE (pressKey (window, '3'));
    CHECK (writeSnapshot (window, slice, "export-placeholder").existsAsFile());

    juce::ScopedJuceInitialiser_GUI gui;
    overdub::ui::MainComponent empty;
    layOut (empty, windowWidth, windowHeight);
    CHECK (writeSnapshot (empty, slice, "no-song").existsAsFile());
}

// Edit mode (issue #28): build/ui-snapshots/slice-07/*.png. Two Passes recorded over a 6 s
// Backing Track, the second a punch-in over the first, so the hatched covered strip shows on
// Guitar and Vocals. Rendered zoomed in and zoomed out, with a Clip selected and the playhead in it.
TEST_CASE ("snapshots of the Edit timeline with overlapping Clips, zoomed in and out", "[ui][snapshot]")
{
    const std::string slice = "slice-07";
    MainWindowFixture f { overdub::song::testing::FixtureSongBuilder {}
                              .backingTrack ("Band Mix", 6.0, 220.0)
                              .track ("Guitar", true, { 0 })
                              .track ("Vocals", true, { 1 })
                              .build() };
    auto& window = *f.window;

    // Pass 1: 0 to about 3 s. Pass 2: about 1 s to 2 s, on top.
    REQUIRE (pressKey (window, 'r'));
    f.runBlocks (560);
    REQUIRE (pressKey (window, juce::KeyPress::spaceKey));
    f.runBlocks (1);
    f.openSong->seek (48000);
    f.runBlocks (0);
    REQUIRE (pressKey (window, 'r'));
    f.runBlocks (190);
    REQUIRE (pressKey (window, juce::KeyPress::spaceKey));
    f.runBlocks (1);
    REQUIRE (f.song().passes.size() == 2);
    REQUIRE (f.trackNamed ("Guitar").clips.size() == 2);

    REQUIRE (pressKey (window, '2'));
    auto& timeline = window.editPage.timeline;
    timeline.selectClip (f.trackNamed ("Guitar").clips[1].id);
    f.openSong->seek (72000);
    f.runBlocks (0);

    timeline.setView (0, 256);
    window.tick();
    CHECK (writeSnapshot (window, slice, "edit-overlap-zoomed-in").existsAsFile());

    timeline.setView (0, 2048);
    window.tick();
    CHECK (writeSnapshot (window, slice, "edit-overlap-zoomed-out").existsAsFile());

    click (window.editPage.toolbar.mixerButton);
    timeline.setView (0, 512);
    window.tick();
    CHECK (writeSnapshot (window, slice, "edit-mixer-hidden").existsAsFile());
}

// Clip gain and fades (issue #29): build/ui-snapshots/slice-08/*.png. A 6 s Backing Track with a
// long fade-in, a fade-out and -6 dB set from the toolbar fields; a recorded Take with short fades
// and +4.5 dB set by dragging its handles and dB tag; the Take's Clip selected.
TEST_CASE ("snapshots of Clips with fades and non-zero gain", "[ui][snapshot][clip_gain]")
{
    const std::string slice = "slice-08";
    MainWindowFixture f { overdub::song::testing::FixtureSongBuilder {}
                              .backingTrack ("Band Mix", 6.0, 220.0)
                              .track ("Guitar", true, { 0 })
                              .build() };
    auto& window = *f.window;

    f.openSong->seek (24000);
    f.runBlocks (0);
    REQUIRE (pressKey (window, 'r'));
    f.runBlocks (560);
    REQUIRE (pressKey (window, juce::KeyPress::spaceKey));
    f.runBlocks (1);
    REQUIRE (f.trackNamed ("Guitar").clips.size() == 1);

    REQUIRE (pressKey (window, '2'));
    auto& page = window.editPage;
    auto& timeline = page.timeline;
    timeline.setView (0, 512);
    window.tick();

    timeline.selectClip (f.trackNamed ("Band Mix").clips[0].id);
    page.toolbar.fadeInField.setText ("1.5", juce::sendNotificationSync);
    page.toolbar.fadeOutField.setText ("2", juce::sendNotificationSync);
    page.toolbar.gainField.setText ("-6", juce::sendNotificationSync);

    const auto guitarClip = f.trackNamed ("Guitar").clips[0].id;
    const auto fadeIn = timeline.handleBounds (guitarClip, overdub::ui::ClipPart::fadeInHandle);
    drag (timeline, fadeIn.getCentre(), fadeIn.getCentre().translated (20, 0));
    const auto fadeOut = timeline.handleBounds (guitarClip, overdub::ui::ClipPart::fadeOutHandle);
    drag (timeline, fadeOut.getCentre(), fadeOut.getCentre().translated (-30, 0));
    const auto tag = timeline.handleBounds (guitarClip, overdub::ui::ClipPart::gainTag);
    drag (timeline, tag.getCentre(), tag.getCentre().translated (0, -45));
    REQUIRE (f.trackNamed ("Guitar").clips[0].fadeInLength > 0);
    REQUIRE (f.trackNamed ("Guitar").clips[0].gainDb > 4.0);
    REQUIRE (f.trackNamed ("Band Mix").clips[0].gainDb < -5.0);

    window.tick();
    CHECK (writeSnapshot (window, slice, "edit-fades-and-gain").existsAsFile());

    timeline.setView (0, 128);
    window.tick();
    CHECK (writeSnapshot (window, slice, "edit-fades-and-gain-zoomed-in").existsAsFile());
}

// Take Lanes and Bring back (issue #30): build/ui-snapshots/slice-09/*.png. Three Passes on a 6 s
// Backing Track: Pass 1 from 0 to about 3 s, Pass 2 about 1 s to 2 s, Pass 3 about 1.5 s to
// 2.5 s, each on top. Guitar's three lanes unfolded (Pass 1 and Pass 2 partly heard, Pass 3
// heard), then the Passes panel open over the timeline.
TEST_CASE ("snapshots of a Track with three Take Lanes unfolded, partly heard, and the Passes panel", "[ui][snapshot][take_lanes]")
{
    const std::string slice = "slice-09";
    MainWindowFixture f { overdub::song::testing::FixtureSongBuilder {}
                              .backingTrack ("Band Mix", 6.0, 220.0)
                              .track ("Guitar", true, { 0 })
                              .track ("Vocals", true, { 1 })
                              .build() };
    auto& window = *f.window;

    const auto recordPass = [&] (overdub::song::SampleCount from, int blocks) {
        f.openSong->seek (from);
        f.runBlocks (0);
        REQUIRE (pressKey (window, 'r'));
        f.runBlocks (blocks);
        REQUIRE (pressKey (window, juce::KeyPress::spaceKey));
        f.runBlocks (1);
    };
    recordPass (0, 560);
    recordPass (48000, 190);
    recordPass (72000, 190);
    REQUIRE (f.song().passes.size() == 3);
    REQUIRE (f.trackNamed ("Guitar").clips.size() == 3);

    REQUIRE (pressKey (window, '2'));
    auto& timeline = window.editPage.timeline;
    const auto& guitarId = f.trackNamed ("Guitar").id;
    timeline.setTakeLanesUnfolded (guitarId, true);
    const auto lanes = timeline.takeLanesShown (guitarId);
    REQUIRE (lanes.size() == 3);
    REQUIRE (lanes[0].status == overdub::song::HeardStatus::heard);
    REQUIRE (lanes[1].status == overdub::song::HeardStatus::partlyHeard);
    REQUIRE (lanes[2].status == overdub::song::HeardStatus::partlyHeard);
    timeline.setView (0, 256);
    window.tick();
    CHECK (writeSnapshot (window, slice, "edit-take-lanes-unfolded").existsAsFile());

    click (window.editPage.passesButton);
    REQUIRE (window.editPage.isPassesPanelOpen());
    window.tick();
    CHECK (writeSnapshot (window, slice, "edit-passes-panel").existsAsFile());
}

// The Mixer (issue #31): build/ui-snapshots/slice-10/*.png. Four Tracks (a 6 s Backing Track and
// three recorded ones) and the Master, with volumes, pans, a mute and a solo set through the
// strips, the Record rows and the Edit headers, and the meters lit by playback; then the Mixer
// hidden, and Record mode's rows with M and S.
TEST_CASE ("snapshots of the Mixer with four strips and the Master, and hidden", "[ui][snapshot][mixer]")
{
    const std::string slice = "slice-10";
    MainWindowFixture f { overdub::song::testing::FixtureSongBuilder {}
                              .backingTrack ("Band Mix", 6.0, 220.0)
                              .track ("Guitar", true, { 0 })
                              .track ("Vocals", true, { 1 })
                              .track ("Keys", true, { 0 })
                              .build() };
    auto& window = *f.window;

    REQUIRE (pressKey (window, 'r'));
    f.runBlocks (560);
    REQUIRE (pressKey (window, juce::KeyPress::spaceKey));
    f.runBlocks (1);
    REQUIRE (f.song().passes.size() == 1);

    REQUIRE (pressKey (window, '2'));
    auto& mixer = window.editPage.mixer;
    REQUIRE (mixer.strips.size() == 4);
    const auto dragFader = [] (overdub::ui::MixerFader& fader, int deltaY) {
        const auto y = overdub::ui::MixerFader::thumbHeight / 2 + (int) std::lround ((1.0 - overdub::ui::dbToFaderPosition (fader.db())) * fader.travel());
        const juce::Point<int> grab { fader.getWidth() / 2, y };
        drag (fader, grab, grab.translated (0, deltaY));
    };
    const auto dragPan = [] (overdub::ui::PanControl& pan, int deltaX) {
        const auto centre = pan.getLocalBounds().getCentre();
        drag (pan, centre, centre.translated (deltaX, 0));
    };
    dragFader (mixer.strips[0]->fader, 20);  // Band Mix down
    dragFader (mixer.strips[1]->fader, -12); // Guitar up
    dragPan (mixer.strips[1]->panControl, -25);
    dragPan (mixer.strips[3]->panControl, 30);
    dragFader (mixer.masterStrip.fader, 8);
    click (mixer.strips[2]->muteButton); // Vocals muted
    click (window.editPage.timeline.headers[3]->soloButton); // Keys soloed from its header
    click (window.editPage.timeline.headers[3]->soloButton); // and unsoloed again
    REQUIRE (f.trackNamed ("Vocals").mute);

    f.openSong->seek (0);
    REQUIRE (pressKey (window, juce::KeyPress::spaceKey));
    f.runBlocks (60);
    window.editPage.timeline.setView (0, 512);
    window.tick();
    REQUIRE (mixer.masterStrip.meter.level() > 0.0f);
    CHECK (writeSnapshot (window, slice, "edit-mixer").existsAsFile());

    click (window.editPage.toolbar.mixerButton);
    window.tick();
    CHECK (writeSnapshot (window, slice, "edit-mixer-hidden").existsAsFile());

    REQUIRE (pressKey (window, juce::KeyPress::spaceKey));
    f.runBlocks (1);
    click (f.rowNamed ("Keys").soloButton);
    REQUIRE (pressKey (window, '1'));
    window.tick();
    CHECK (writeSnapshot (window, slice, "record-mute-solo").existsAsFile());
}

// Record mode complete (issue #32): build/ui-snapshots/slice-11/*.png. An 8 s Band Mix and a 6 s
// Outro moved to 12 s leave a break the strip counts down to; Pass 1 (from 1 s) is in the Passes
// list. With pre-roll 2 s, R from 5 s: once 1 s into the pre-roll, and once 1.5 s into the Pass.
TEST_CASE ("snapshots of Record mode during a pre-roll countdown and during a Pass", "[ui][snapshot][pre_roll]")
{
    const std::string slice = "slice-11";
    MainWindowFixture f { overdub::song::testing::FixtureSongBuilder {}
                              .backingTrack ("Band Mix", 8.0, 220.0)
                              .backingTrack ("Outro", 6.0, 330.0)
                              .track ("Guitar", true, { 0 })
                              .track ("Vocals", true, { 1 })
                              .build() };
    auto& window = *f.window;
    auto moved = f.song();
    moved.tracks[1].clips[0].timelineStart = 12 * 48000;
    REQUIRE (f.openSong->edit (moved, "Move Clip").has_value());

    f.openSong->seek (48000);
    f.runBlocks (0);
    REQUIRE (pressKey (window, 'r'));
    f.runBlocks (380);
    REQUIRE (pressKey (window, juce::KeyPress::spaceKey));
    f.runBlocks (1);
    REQUIRE (f.song().passes.size() == 1);

    choose (window.recordPage.preRollPicker, overdub::ui::preRollChoiceIndex (overdub::song::PreRoll::twoSeconds));
    f.openSong->seek (5 * 48000);
    f.runBlocks (0);
    REQUIRE (pressKey (window, 'r'));
    f.runBlocks (188); // about 1 s of the 2 s pre-roll
    REQUIRE (f.openSong->preRollRecordPoint().has_value());
    CHECK (writeSnapshot (window, slice, "record-pre-roll").existsAsFile());

    f.runBlocks (470); // about 1.5 s into the Pass
    REQUIRE (f.openSong->runningPassStart().has_value());
    CHECK (writeSnapshot (window, slice, "record-pass").existsAsFile());

    REQUIRE (pressKey (window, juce::KeyPress::spaceKey));
    f.runBlocks (1);
    REQUIRE (f.song().passes.size() == 2);
    CHECK (writeSnapshot (window, slice, "record-idle-two-passes").existsAsFile());
}

// The camera (issue #35): build/ui-snapshots/slice-14/*.png. Record mode with the camera
// disarmed, then Armed (the fake camera has no live feed, so the stage shows its armed line), and
// during a Pass with the camera.
TEST_CASE ("snapshots of Record mode's camera arm, picker and stage", "[ui][snapshot][camera]")
{
    const std::string slice = "slice-14";
    MainWindowFixture f;
    auto& window = *f.window;
    window.tick();
    CHECK (writeSnapshot (window, slice, "record-camera-disarmed").existsAsFile());

    click (window.recordPage.cameraArmButton);
    REQUIRE (f.song().cameraArmed);
    CHECK (writeSnapshot (window, slice, "record-camera-armed").existsAsFile());

    f.camera.movie.frames = 10;
    REQUIRE (pressKey (window, 'r'));
    f.runBlocks (200);
    REQUIRE (f.openSong->isRecording());
    CHECK (writeSnapshot (window, slice, "record-camera-pass").existsAsFile());
    REQUIRE (pressKey (window, juce::KeyPress::spaceKey));
    f.runBlocks (1);
}

// Video Take playback (issue #36): build/ui-snapshots/slice-15/*.png. Two Video Takes (Pass 1's 4 s
// from 1 s, Pass 2's 2 s from 3 s) on the Camera lane in Edit mode, with the playhead in their
// overlap; the Video Preview's content, and the Record stage showing playback with the playhead
// outside every Video Take. The AVSampleBufferDisplayLayer area renders blank off-screen; which
// frame shows is covered by the pure and Component tests.
TEST_CASE ("snapshots of Edit mode with the Camera lane and the Video Preview", "[ui][snapshot][video]")
{
    const std::string slice = "slice-15";
    MainWindowFixture f;
    auto& window = *f.window;
    auto changed = f.song();
    const auto root = f.openSong->package().location();
    overdub::video::testing::addSyntheticVideoTake (changed, root, 48000, 120);
    overdub::video::testing::addSyntheticVideoTake (changed, root, 3 * 48000, 60);
    REQUIRE (f.openSong->edit (changed, "Add Video Takes").has_value());

    REQUIRE (pressKey (window, '2'));
    window.editPage.timeline.setView (0, 250);
    f.openSong->seek (4 * 48000);
    f.runBlocks (1);
    click (window.editPage.toolbar.videoButton);
    window.refreshVideo (f.device.hostTimeNs);
    REQUIRE (window.videoPreviewWindow() != nullptr);
    CHECK (writeSnapshot (window, slice, "edit-camera-lane").existsAsFile());
    CHECK (writeSnapshot (window.videoPreviewWindow()->screen(), slice, "video-preview").existsAsFile());

    f.openSong->seek (8 * 48000);
    f.runBlocks (1);
    window.refreshVideo (f.device.hostTimeNs);
    CHECK (writeSnapshot (window.videoPreviewWindow()->screen(), slice, "video-preview-no-video-take").existsAsFile());

    REQUIRE (pressKey (window, '1'));
    window.refreshVideo (f.device.hostTimeNs);
    CHECK (writeSnapshot (window, slice, "record-stage-playback").existsAsFile());
}
