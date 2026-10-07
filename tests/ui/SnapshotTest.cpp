#include "ui/MainWindowFixture.h"

#include <catch2/catch_test_macros.hpp>

using namespace overdub::ui::testing;

// Renders each surface of this slice for a session to look at (ADR 0005 UI amendment):
// build/ui-snapshots/slice-06/*.png. Nothing here asserts on pixels.
TEST_CASE ("snapshots of Record mode, idle and recording, and the Edit and Export placeholders", "[ui][snapshot]")
{
    const std::string slice = "slice-06";
    MainWindowFixture f;
    auto& window = *f.window;
    window.tick();

    CHECK (writeSnapshot (window, slice, "record-idle").existsAsFile());

    REQUIRE (pressKey (window, 'r'));
    f.runBlocks (400); // about 2.1 s at 48 kHz
    REQUIRE (f.session->isRecording());
    CHECK (writeSnapshot (window, slice, "record-recording").existsAsFile());
    REQUIRE (pressKey (window, juce::KeyPress::spaceKey));
    f.runBlocks (1);
    REQUIRE (! f.session->isRecording());

    REQUIRE (pressKey (window, '2'));
    CHECK (writeSnapshot (window, slice, "edit-placeholder").existsAsFile());
    REQUIRE (pressKey (window, '3'));
    CHECK (writeSnapshot (window, slice, "export-placeholder").existsAsFile());

    juce::ScopedJuceInitialiser_GUI gui;
    overdub::ui::MainComponent empty;
    layOut (empty, windowWidth, windowHeight);
    CHECK (writeSnapshot (empty, slice, "no-song").existsAsFile());
}
