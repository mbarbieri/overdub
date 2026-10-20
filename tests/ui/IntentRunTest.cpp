#include "ui/MainComponent.h"

#include "core/LogEvents.h"
#include "song/MixEdits.h"
#include "ui/MainWindowFixture.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <algorithm>
#include <cmath>
#include <functional>
#include <string>
#include <vector>

using namespace overdub;
using namespace overdub::ui;
using namespace overdub::ui::testing;
using Catch::Matchers::WithinAbs;

namespace
{
std::vector<nlohmann::json> eventsNamed (const core::ScopedLogCapture& capture, const std::string& name)
{
    std::vector<nlohmann::json> found;
    for (const auto& event : capture.events())
        if (event.at ("event") == name)
            found.push_back (event);
    return found;
}

bool anythingPresented (const core::ScopedLogCapture& capture)
{
    return ! eventsNamed (capture, "ui.notice_shown").empty() || ! eventsNamed (capture, "ui.alert_shown").empty();
}

IntentResult runFromSocket (MainWindowFixture& f, SongIntent intent)
{
    return f.window->run (std::move (intent), Origin::control);
}

IntentResult runEdit (MainWindowFixture& f, intent::Edit edit)
{
    return runFromSocket (f, SongIntent { std::move (edit) });
}

float recentPeak (const engine::testing::LoopbackDevice& device, std::size_t samples)
{
    const auto& output = device.outputHistory[0];
    float peak = 0.0f;
    for (auto i = output.size() - samples; i < output.size(); ++i)
        peak = std::max (peak, std::abs (output[i]));
    return peak;
}

MixerStrip& stripNamed (MainWindowFixture& f, const std::string& name)
{
    const auto& trackId = f.trackNamed (name).id;
    for (auto& strip : f.window->songView()->editPage.mixer.strips)
        if (strip->trackId() == trackId)
            return *strip;
    FAIL ("no strip for Track " << name);
    return *f.window->songView()->editPage.mixer.strips.front();
}

void checkQuietError (const core::ScopedLogCapture& capture, const IntentResult& result, const std::string& code)
{
    REQUIRE_FALSE (result.has_value());
    CHECK (result.error().code == code);
    const auto logged = eventsNamed (capture, code);
    REQUIRE (logged.size() == 1);
    CHECK (logged[0].at ("lvl") == "debug");
    CHECK_FALSE (anythingPresented (capture));
}
} // namespace

TEST_CASE ("every run logs ui.intent_run with its op and where it came from", "[ui][intent]")
{
    MainWindowFixture f;
    core::ScopedLogCapture capture;

    REQUIRE (pressKey (*f.window, '2'));
    runFromSocket (f, intent::Rewind {});

    const auto runs = eventsNamed (capture, "ui.intent_run");
    REQUIRE (runs.size() == 2);
    CHECK (runs[0].at ("lvl") == "debug");
    CHECK (runs[0].at ("op") == "show_mode");
    CHECK (runs[0].at ("origin") == "user");
    CHECK (runs[1].at ("op") == "rewind");
    CHECK (runs[1].at ("origin") == "control");
}

TEST_CASE ("a pending Mixer arrow-key burst followed by a mute keeps both, as two undo steps", "[ui][intent][mixer]")
{
    MainWindowFixture f;
    REQUIRE (pressKey (*f.window, '2'));
    const auto now = song::SongPackage::Clock::now();
    f.window->clock = [now] { return now; };
    const auto guitarId = f.trackNamed ("Guitar").id;
    const auto bandMixId = f.trackNamed ("Band Mix").id;
    auto& fader = stripNamed (f, "Guitar").fader;
    clickAt (fader, fader.thumbCentre());
    REQUIRE (pressKey (*f.window, juce::KeyPress (juce::KeyPress::downKey, {}, 0)));
    REQUIRE (pressKey (*f.window, juce::KeyPress (juce::KeyPress::downKey, {}, 0)));
    REQUIRE_THAT (song::findTrack (f.song(), guitarId)->volumeDb, WithinAbs (0.0, 1.0e-9));

    SECTION ("the mute from the socket, logged as one control run")
    {
        core::ScopedLogCapture capture;
        CHECK (runEdit (f, intent::SetTrackMute { bandMixId, true }).has_value());
        const auto runs = eventsNamed (capture, "ui.intent_run");
        REQUIRE (runs.size() == 1);
        CHECK (runs[0].at ("op") == "set_track_mute");
        CHECK (runs[0].at ("origin") == "control");
    }
    SECTION ("the mute from the Band Mix strip's M")
    {
        click (stripNamed (f, "Band Mix").muteButton);
    }

    CHECK_THAT (song::findTrack (f.song(), guitarId)->volumeDb, WithinAbs (-2.0, 1.0e-9));
    CHECK (f.trackNamed ("Band Mix").mute);
    CHECK (f.openSong->package().undoLabel() == "Mute Band Mix");
    REQUIRE (pressKey (*f.window, 'z', juce::ModifierKeys::commandModifier));
    CHECK_FALSE (f.trackNamed ("Band Mix").mute);
    CHECK_THAT (song::findTrack (f.song(), guitarId)->volumeDb, WithinAbs (-2.0, 1.0e-9));
    CHECK (f.openSong->package().undoLabel() == "Guitar Volume");
}

TEST_CASE ("a pending Mixer arrow-key burst is committed by a mode change from the socket, logged as that one control run", "[ui][intent][mixer]")
{
    MainWindowFixture f;
    REQUIRE (pressKey (*f.window, '2'));
    const auto guitarId = f.trackNamed ("Guitar").id;
    auto& fader = stripNamed (f, "Guitar").fader;
    clickAt (fader, fader.thumbCentre());
    REQUIRE (pressKey (*f.window, juce::KeyPress (juce::KeyPress::downKey, {}, 0)));
    core::ScopedLogCapture capture;

    CHECK (f.window->run (WindowIntent { intent::ShowMode { Mode::record } }, Origin::control).has_value());

    CHECK_THAT (song::findTrack (f.song(), guitarId)->volumeDb, WithinAbs (-1.0, 1.0e-9));
    CHECK (f.openSong->package().undoLabel() == "Guitar Volume");
    const auto runs = eventsNamed (capture, "ui.intent_run");
    REQUIRE (runs.size() == 1);
    CHECK (runs[0].at ("op") == "show_mode");
    CHECK (runs[0].at ("origin") == "control");
}

TEST_CASE ("a Mixer fader drag logs one run, for the edit its release commits, and none per frame", "[ui][intent][mixer]")
{
    MainWindowFixture f;
    REQUIRE (pressKey (*f.window, '2'));
    auto& fader = stripNamed (f, "Guitar").fader;
    const auto grab = fader.thumbCentre();
    core::ScopedLogCapture capture;

    drag (fader, grab, grab.translated (0, -30));

    const auto runs = eventsNamed (capture, "ui.intent_run");
    REQUIRE (runs.size() == 1);
    CHECK (runs[0].at ("op") == "set_track_volume");
    CHECK (runs[0].at ("origin") == "user");
}

TEST_CASE ("a mix preview from the socket is heard without changing the Song, and an empty one goes back to the Song", "[ui][intent][mixer]")
{
    MainWindowFixture f;
    const auto block = (std::size_t) deviceBlockSize;
    const auto before = f.song();
    const auto labelBefore = f.openSong->package().undoLabel();
    REQUIRE (f.openSong->playOrStop().has_value());
    f.device.run (f.engine, 4);
    const auto loud = recentPeak (f.device, block);
    REQUIRE (loud > 0.0f);

    CHECK (runFromSocket (f, intent::PreviewMix { intent::SetMasterVolume { song::minVolumeDb } }).has_value());
    f.device.run (f.engine, 4);
    CHECK (recentPeak (f.device, block) < loud / 100.0f);
    CHECK (f.song() == before);
    CHECK (f.openSong->package().undoLabel() == labelBefore);

    CHECK (runFromSocket (f, intent::PreviewMix {}).has_value());
    f.device.run (f.engine, 4);
    CHECK (recentPeak (f.device, block) > loud / 2.0f);
}

TEST_CASE ("delete_track during a Pass returns ui.stop_recording_first and changes nothing", "[ui][intent][delete_track]")
{
    MainWindowFixture f;
    REQUIRE (pressKey (*f.window, 'r'));
    REQUIRE (f.openSong->isRecording());
    const auto before = f.song();
    core::ScopedLogCapture capture;

    const auto result = runFromSocket (f, intent::DeleteTrack { f.trackNamed ("Guitar").id });

    checkQuietError (capture, result, "ui.stop_recording_first");
    CHECK (f.song() == before);
    CHECK (eventsNamed (capture, "ui.track_deleted").empty());
    REQUIRE (pressKey (*f.window, juce::KeyPress::spaceKey));
    f.runBlocks (1);
}

TEST_CASE ("delete_track from the socket deletes the Track and turns its Monitoring off", "[ui][intent][delete_track]")
{
    MainWindowFixture f;
    const auto guitarId = f.trackNamed ("Guitar").id;
    REQUIRE (f.openSong->setMonitoring (guitarId, true).has_value());
    core::ScopedLogCapture capture;

    CHECK (runFromSocket (f, intent::DeleteTrack { guitarId }).has_value());

    CHECK (song::findTrack (f.song(), guitarId) == nullptr);
    CHECK (f.openSong->package().undoLabel() == "Delete Track Guitar");
    CHECK (eventsNamed (capture, "ui.track_deleted").size() == 1);
    REQUIRE (pressKey (*f.window, 'z', juce::ModifierKeys::commandModifier));
    REQUIRE (song::findTrack (f.song(), guitarId) != nullptr);
    CHECK_FALSE (f.openSong->isMonitoring (guitarId));
}

TEST_CASE ("split_selected_clip splits at the playhead, selects the right half and returns its id", "[ui][intent][edit]")
{
    MainWindowFixture f;
    REQUIRE (pressKey (*f.window, '2'));
    const auto clip = f.trackNamed ("Band Mix").clips.at (0);
    const auto at = clip.timelineStart + clip.length / 2;
    auto& timeline = f.window->songView()->editPage.timeline;
    CHECK (runFromSocket (f, intent::SelectClip { clip.id }).has_value());
    REQUIRE (timeline.selectedClip() == clip.id);
    CHECK (runFromSocket (f, intent::Seek { at }).has_value());

    const auto result = runFromSocket (f, intent::SplitSelectedClip {});

    REQUIRE (result.has_value());
    REQUIRE (result->createdId.has_value());
    CHECK (timeline.selectedClip() == result->createdId);
    const auto& clips = f.trackNamed ("Band Mix").clips;
    REQUIRE (clips.size() == 2);
    const auto rightHalf = std::ranges::find (clips, *result->createdId, &song::Clip::id);
    REQUIRE (rightHalf != clips.end());
    CHECK (rightHalf->timelineStart == at);
    CHECK (f.openSong->package().undoLabel() == "Split Clip");
}

TEST_CASE ("record clears every Record row's clip indicator as the Pass starts", "[ui][intent][record]")
{
    MainWindowFixture f;
    auto& clip = f.rowNamed ("Guitar").clip;
    clip.light();

    CHECK (runFromSocket (f, intent::Record {}).has_value());

    CHECK (f.openSong->isRecording());
    CHECK_FALSE (clip.isLit());
    REQUIRE (pressKey (*f.window, juce::KeyPress::spaceKey));
    f.runBlocks (1);
}

TEST_CASE ("add_track returns the new Track's id", "[ui][intent][tracks]")
{
    MainWindowFixture f;

    const auto result = runFromSocket (f, intent::AddTrack {});

    REQUIRE (result.has_value());
    REQUIRE (result->createdId.has_value());
    CHECK (f.song().tracks.back().id == *result->createdId);
}

TEST_CASE ("a gesture that does nothing returns a quiet Error naming why", "[ui][intent]")
{
    MainWindowFixture f;
    core::ScopedLogCapture capture;

    SECTION ("S outside Edit mode")
    {
        checkQuietError (capture, runFromSocket (f, intent::SplitSelectedClip {}), "ui.not_in_edit_mode");
    }
    SECTION ("S with no Clip selected")
    {
        REQUIRE (pressKey (*f.window, '2'));
        checkQuietError (capture, runFromSocket (f, intent::SplitSelectedClip {}), "ui.no_clip_selected");
    }
    SECTION ("backspace with nothing selected")
    {
        REQUIRE (pressKey (*f.window, '2'));
        checkQuietError (capture, runFromSocket (f, intent::DeleteSelection {}), "ui.nothing_selected");
    }
    SECTION ("redo with nothing to redo")
    {
        checkQuietError (capture, runFromSocket (f, intent::Redo {}), "ui.nothing_to_redo");
    }
    SECTION ("a Track moved to its own slot")
    {
        checkQuietError (capture, runFromSocket (f, intent::MoveTrack { f.trackNamed ("Guitar").id, 1 }), "ui.track_already_there");
    }
    SECTION ("a dismissed import")
    {
        checkQuietError (capture, runFromSocket (f, intent::ImportFiles {}), "ui.no_files_to_import");
    }
}

TEST_CASE ("undo with nothing to undo returns ui.nothing_to_undo", "[ui][intent]")
{
    MainWindowFixture f;
    while (f.openSong->package().canUndo())
        REQUIRE (f.openSong->undo().has_value());
    core::ScopedLogCapture capture;

    checkQuietError (capture, runFromSocket (f, intent::Undo {}), "ui.nothing_to_undo");
}

TEST_CASE ("a camera arm a Pass refuses returns the refusal without asking for camera access", "[ui][intent][camera]")
{
    MainWindowFixture f;
    int accessAsked = 0;
    f.window->askCameraAccess = [&accessAsked] (std::function<void (bool)> done) {
        ++accessAsked;
        done (true);
    };
    REQUIRE (pressKey (*f.window, 'r'));
    core::ScopedLogCapture capture;

    checkQuietError (capture, runFromSocket (f, intent::SetCameraArmed { true }), "ui.stop_recording_first");
    CHECK (accessAsked == 0);
    REQUIRE (pressKey (*f.window, juce::KeyPress::spaceKey));
    f.runBlocks (1);
}

TEST_CASE ("a Song Intent with no Song open returns ui.no_song_open", "[ui][intent]")
{
    MainWindowFixture f;
    f.window->setOpenSong (nullptr);
    core::ScopedLogCapture capture;

    checkQuietError (capture, runFromSocket (f, intent::PlayOrStop {}), "ui.no_song_open");
    CHECK (f.window->run (WindowIntent { intent::ShowMode { Mode::edit } }).has_value());
    CHECK (f.window->mode() == Mode::edit);
}
