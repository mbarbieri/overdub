#pragma once

#include "ui/MainComponent.h"
#include "ui/OpenSong.h"

#include "capture/FakeCamera.h"
#include "engine/EngineFixture.h"
#include "song/FixtureSongBuilder.h"
#include "ui/UiHarness.h"

#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

// The main window's content over a fixture Song (ADR 0005 UI amendment): a real MainComponent at
// a fixed size, bound through a real OpenSong to an Engine that a loopback fake device drives.
// Alerts, confirmations, file choosers and camera access are injected; nothing opens a window, a
// dialog, a camera or audio hardware (plugin editor windows are built off the desktop; the camera
// is a fake).
namespace overdub::ui::testing
{
inline constexpr int windowWidth = 1200;
inline constexpr int windowHeight = 760;
inline constexpr int deviceBlockSize = 256;

struct MainWindowFixture
{
    juce::ScopedJuceInitialiser_GUI gui;
    std::unique_ptr<song::testing::FixtureSong> fixture;
    engine::Engine engine { engine::EngineConfig { .numOutputChannels = 2, .maxBlockSize = deviceBlockSize } };
    engine::testing::LoopbackDevice device { 2, deviceBlockSize, 512 };
    capture::testing::FakeCamera camera; // before openSong, which uses it until it closes
    bool cameraAccessGranted = true;
    std::unique_ptr<OpenSong> openSong;
    std::unique_ptr<MainComponent> window;
    std::vector<juce::String> alertsShown;
    // Per Alert shown, in order: the user's OK (its `closed`), and how many times the window
    // closed it (dismiss_alerts).
    std::vector<std::function<void()>> closeAlertAsUser;
    std::vector<int> alertClosesByWindow;
    std::vector<juce::String> deleteQuestionsAsked;
    // The open delete confirmation. Like the native dialog it is answered later, never inside the
    // click that asked (which would delete the clicked header under its own button).
    std::function<void (bool)> pendingDeleteAnswer;

    explicit MainWindowFixture (std::unique_ptr<song::testing::FixtureSong> songFixture = song::testing::threeTrackFixture(),
                                plugin::Instantiator instantiate = {},
                                std::vector<std::string> inputChannelNames = { "In 1", "In 2", "In 3", "In 4" })
        : fixture (std::move (songFixture))
    {
        openSong = std::make_unique<OpenSong> (std::move (fixture->package), AudioBinding { &engine, {}, std::move (inputChannelNames) }, std::move (instantiate));
        window = std::make_unique<MainComponent>();
        REQUIRE (openSong->attachCamera (&camera, juce::File {}).has_value()); // no calibration file: 0 ms
        window->showAlert = [this] (const juce::String& sentence, std::function<void()> closed) {
            const auto index = alertsShown.size();
            alertsShown.push_back (sentence);
            closeAlertAsUser.push_back (std::move (closed));
            alertClosesByWindow.push_back (0);
            return [this, index] { ++alertClosesByWindow[index]; };
        };
        window->confirmTrackDelete = [this] (const juce::String& question, std::function<void (bool)> done) {
            deleteQuestionsAsked.push_back (question);
            pendingDeleteAnswer = std::move (done);
        };
        window->askCameraAccess = [this] (std::function<void (bool)> done) { done (cameraAccessGranted); };
        window->showPluginWindowsOnDesktop = false; // editor windows are built, never shown
        layOut (*window, windowWidth, windowHeight);
        window->setOpenSong (openSong.get());
    }

    ~MainWindowFixture()
    {
        window.reset();
        openSong.reset(); // closes the package before the fixture deletes its folder
    }

    const song::Song& song() const { return openSong->song(); }

    const song::Track& trackNamed (const std::string& name) const
    {
        const auto found = std::ranges::find (song().tracks, name, &song::Track::name);
        REQUIRE (found != song().tracks.end());
        return *found;
    }

    TrackRow& rowNamed (const std::string& name)
    {
        const auto& trackId = trackNamed (name).id;
        for (auto& row : window->songView()->recordPage.rows)
            if (row->trackId() == trackId)
                return *row;
        FAIL ("no row for Track " << name);
        return *window->songView()->recordPage.rows.front();
    }

    // The Track's header on the Edit page's Timeline (shown in Edit mode).
    TrackHeader& editHeaderNamed (const std::string& name)
    {
        const auto& trackId = trackNamed (name).id;
        for (auto& header : window->songView()->editPage.timeline.headers)
            if (header->trackId() == trackId)
                return *header;
        FAIL ("no Edit header for Track " << name);
        return *window->songView()->editPage.timeline.headers.front();
    }

    // Delete or Cancel in the open delete confirmation.
    void answerDeleteQuestion (bool confirmed)
    {
        REQUIRE (pendingDeleteAnswer != nullptr);
        auto answer = std::exchange (pendingDeleteAnswer, nullptr);
        answer (confirmed);
    }

    // Runs device callbacks, then one window poll, as the timer would.
    void runBlocks (int blocks)
    {
        device.run (engine, blocks);
        window->tick();
    }
};
} // namespace overdub::ui::testing
