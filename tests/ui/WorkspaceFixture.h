#pragma once

#include "ui/Workspace.h"

#include "capture/FakeCamera.h"
#include "core/Log.h"
#include "song/FixtureSongBuilder.h"
#include "ui/FakeLiveAudio.h"
#include "ui/UiHarness.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <memory>
#include <optional>
#include <string>
#include <vector>

// The main window's content with its ui::Workspace (issue #105), driven with the test LiveAudio:
// devices are lost, reconfigured or come back by the test's word, the calibration files live in a
// temp folder, and the Workspace's window hooks (the Settings window, quitting) are recorded.
// Nothing opens a window, a dialog, a camera or audio hardware.
namespace overdub::ui::testing
{
inline std::vector<nlohmann::json> eventsNamed (const core::ScopedLogCapture& capture, const std::string& name)
{
    std::vector<nlohmann::json> found;
    for (const auto& event : capture.events())
        if (event.at ("event") == name)
            found.push_back (event);
    return found;
}

// The codes of the Notices and Alerts the content showed.
inline std::vector<std::string> messageCodes (const core::ScopedLogCapture& capture)
{
    std::vector<std::string> codes;
    for (const auto& event : capture.events())
        if (event.at ("event") == "ui.notice_shown" || event.at ("event") == "ui.alert_shown")
            codes.push_back (event.at ("code"));
    return codes;
}

inline bool contains (const std::vector<std::string>& codes, const std::string& code)
{
    return std::ranges::contains (codes, code);
}

struct WorkspaceFixture
{
    juce::ScopedJuceInitialiser_GUI gui;
    juce::File folder = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("overdub-workspace-" + juce::Uuid().toString());
    WorkspaceFiles files { folder.getChildFile ("device.json"), folder.getChildFile ("latency.json"), folder.getChildFile ("camera.json") };
    FakeMachine machine;
    capture::testing::FakeCamera camera;
    MainComponent content;
    DeviceSettingsPanel devicePanel;
    CalibrationPanel calibrationPanel;
    // Every LiveAudio the Workspace was given or opened, oldest first.
    std::vector<FakeLiveAudio*> opened;
    std::vector<std::optional<int>> openedForRates;
    // What the Workspace asked of the window: each show_settings answer, and each quit.
    std::vector<bool> settingsShown;
    int quits = 0;
    std::unique_ptr<song::testing::FixtureSong> fixture; // its folder outlives the Workspace's package
    std::unique_ptr<Workspace> workspace;

    WorkspaceFixture()
    {
        REQUIRE (folder.createDirectory());
        machine.latencyCalibrationFile = files.latencyCalibration;
        content.showAlert = [] (const juce::String&, std::function<void()>) { return MainComponent::CloseAlert {}; };
        content.askCameraAccess = [] (std::function<void (bool)> done) { done (true); };
        content.showPluginWindowsOnDesktop = false;
        content.showVideoPreviewOnDesktop = false;
        layOut (content, 1200, 760);
        layOut (calibrationPanel, 460, 520);
    }

    ~WorkspaceFixture()
    {
        workspace.reset();
        fixture.reset();
        folder.deleteRecursively();
    }

    std::unique_ptr<LiveAudio> openAudio (std::optional<int> songSampleRate)
    {
        auto audio = std::make_unique<FakeLiveAudio> (machine, songSampleRate);
        opened.push_back (audio.get());
        openedForRates.push_back (songSampleRate);
        return audio;
    }

    // Launches with the machine as it is now. The settings panels are attached, as an open Settings
    // window's are, unless `settingsWindowOpen` is false: then show_settings attaches them, as the
    // app's window does when it first builds the Settings window.
    void launch (bool settingsWindowOpen = true)
    {
        workspace = std::make_unique<Workspace> (
            content,
            openAudio (std::nullopt),
            [this] (std::optional<int> rate) { return openAudio (rate); },
            &camera,
            plugin::Instantiator {},
            files);
        workspace->onShowSettings = [this] (bool shown) {
            settingsShown.push_back (shown);
            if (shown)
                workspace->attachSettingsPanels (&devicePanel, &calibrationPanel);
        };
        workspace->onQuit = [this] { ++quits; };
        if (settingsWindowOpen)
            workspace->attachSettingsPanels (&devicePanel, &calibrationPanel);
    }

    void openSong (int sampleRate = engine::testing::testSampleRate)
    {
        fixture = song::testing::FixtureSongBuilder {}.sampleRate (sampleRate).backingTrack ("Band Mix").track ("Guitar", true, { 0 }).build();
        workspace->showSong (std::move (fixture->package), {});
    }

    FakeLiveAudio& audio() { return *opened.back(); }
    OpenSong& song() { return *workspace->openSong(); }

    IntentResult run (const AppIntent& intent) { return content.run (intent, Origin::control); }

    // One tick of each window timer: the content's poll, then the Workspace's.
    void tick (song::SongPackage::Clock::time_point now = song::SongPackage::Clock::now())
    {
        content.tick (now);
        workspace->tick (now);
    }

    void runBlocks (int blocks)
    {
        audio().run (blocks);
        tick();
    }
};
} // namespace overdub::ui::testing
