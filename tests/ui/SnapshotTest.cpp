#include "ui/MainWindowFixture.h"

#include <catch2/catch_test_macros.hpp>

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
