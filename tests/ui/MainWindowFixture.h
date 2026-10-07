#pragma once

#include "ui/MainComponent.h"
#include "ui/SongSession.h"

#include "engine/EngineFixture.h"
#include "song/FixtureSongBuilder.h"
#include "ui/UiHarness.h"

#include <memory>
#include <string>
#include <vector>

// The main window's content over a fixture Song (ADR 0005 UI amendment): a real MainComponent at
// a fixed size, bound through a real SongSession to an Engine that a loopback fake device drives.
// Alerts and file choosers are injected; nothing opens a window, a dialog or audio hardware.
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
    std::unique_ptr<SongSession> session;
    std::unique_ptr<MainComponent> window;
    std::vector<juce::String> alertsShown;

    explicit MainWindowFixture (std::unique_ptr<song::testing::FixtureSong> songFixture = song::testing::threeTrackFixture())
        : fixture (std::move (songFixture))
    {
        session = std::make_unique<SongSession> (std::move (fixture->package), AudioBinding { &engine, {}, { "In 1", "In 2", "In 3", "In 4" } });
        window = std::make_unique<MainComponent>();
        window->showAlert = [this] (const juce::String& sentence) { alertsShown.push_back (sentence); };
        layOut (*window, windowWidth, windowHeight);
        window->setSession (session.get());
    }

    ~MainWindowFixture()
    {
        window.reset();
        session.reset(); // closes the package before the fixture deletes its folder
    }

    const song::Song& song() const { return session->song(); }

    const song::Track& trackNamed (const std::string& name) const
    {
        const auto found = std::ranges::find (song().tracks, name, &song::Track::name);
        REQUIRE (found != song().tracks.end());
        return *found;
    }

    TrackRow& rowNamed (const std::string& name)
    {
        const auto& trackId = trackNamed (name).id;
        for (auto& row : window->recordPage.rows)
            if (row->trackId() == trackId)
                return *row;
        FAIL ("no row for Track " << name);
        return *window->recordPage.rows.front();
    }

    // Runs device callbacks, then one window poll, as the timer would.
    void runBlocks (int blocks)
    {
        device.run (engine, blocks);
        window->tick();
    }
};
} // namespace overdub::ui::testing
