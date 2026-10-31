#include "ui/Activity.h"

#include "core/LogEvents.h"
#include "separation/OnnxTestModel.h"
#include "song/FakeSeparationHelper.h"
#include "ui/Availability.h"
#include "ui/MainWindowFixture.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/generators/catch_generators_range.hpp>

#include <array>
#include <functional>
#include <memory>
#include <string>
#include <vector>

// OpenSong runs one activity at a time, and ui/Availability.h refuses gestures by one table
// (issues #145, #255): every gesture against every activity, run as an Intent through the real
// window and OpenSong over a fake device, a fake camera and a fake Separation helper (ADR 0007: no
// test runs the real model).
using overdub::core::ScopedLogCapture;
using overdub::core::testing::firstEvent;
using namespace overdub::ui;
using namespace overdub::ui::testing;
using namespace overdub::song::testing;
namespace song = overdub::song;
namespace core = overdub::core;
namespace exporting = overdub::exporting;

namespace
{
enum class Started
{
    nothing,
    pass,
    separation,
    conversion,
    exporting,
};

const overdub::plugin::AvailableAudioUnit testPlugin { .component = { "aufx", "test", "ovdb" }, .name = "Test", .manufacturer = "Overdub", .version = "1.0.0" };

// Band Mix at the helper's rate, so it can be separated, and an Armed Guitar Track to record.
std::unique_ptr<FixtureSong> bandMixSong()
{
    return FixtureSongBuilder {}.sampleRate (helperSampleRate).backingTrack ("Band Mix", 2.0, 220.0).track ("Guitar", true, { 0 }).build();
}

constexpr int otherSampleRate = 48000;

song::Song withMasterDb (song::Song song, double volumeDb)
{
    song.master.volumeDb = volumeDb;
    return song;
}

struct ActivityFixture
{
    MainWindowFixture f { bandMixSong() };

    // A history with one step to undo and one to redo, then `running` started. Nothing polls, so
    // the activity runs until the test ends.
    explicit ActivityFixture (Started running)
    {
        f.openSong->setSeparationTools (song::SeparationTools {
            .model = writeStandInModel (f.fixture->folder),
            .startHelper = fakeSeparationHelper ({}, std::make_shared<FakeHelperLog>()),
            .workFolder = f.fixture->folder.getChildFile ("work"),
        });
        REQUIRE (f.openSong->edit (withMasterDb (f.song(), -6.0), "Master -6").has_value());
        REQUIRE (f.openSong->edit (withMasterDb (f.song(), -12.0), "Master -12").has_value());
        REQUIRE (f.openSong->undo().has_value());

        switch (running)
        {
            case Started::nothing: break;
            case Started::pass: REQUIRE (f.openSong->record().has_value()); break;
            case Started::separation: REQUIRE (f.openSong->startSeparation (bandMixId()).has_value()); break;
            case Started::conversion:
                REQUIRE (f.openSong->startSampleRateConversion (otherSampleRate).has_value());
                REQUIRE (f.openSong->isConverting());
                break;
            case Started::exporting:
                REQUIRE (f.openSong->startExport ({ exporting::ExportFormat::wav, f.fixture->folder.getChildFile ("Song.wav") }).has_value());
                REQUIRE (f.openSong->isExporting()); // waits for the audio thread, which no block runs
                break;
        }
    }

    song::Id bandMixId() const { return f.trackNamed ("Band Mix").id; }
    song::Id guitarId() const { return f.trackNamed ("Guitar").id; }
};

struct GestureCase
{
    std::string name;
    std::function<SongIntent (ActivityFixture&)> intent;
    // What run answers with a Pass and with an export running: "allowed", or the refusal's code.
    // Nothing running allows every one, and a Separation's or a conversion's run dialog refuses
    // every one (ui.run_dialog_open).
    std::string duringPass;
    std::string duringExport;
};

const std::string allowed = "allowed";
const std::string pass = "ui.stop_recording_first";
const std::string exportRunning = "ui.export_running";
const std::string runDialog = "ui.run_dialog_open";

// "allowed", or the code of the refusal run answered with.
std::string outcomeOf (const IntentResult& result)
{
    if (result.has_value())
        return allowed;
    const auto& code = result.error().code;
    return code == pass || code == exportRunning || code == runDialog ? code : allowed;
}

std::vector<GestureCase> gestureCases()
{
    using namespace intent;
    return {
        { "record", [] (ActivityFixture&) { return Record {}; }, allowed, exportRunning },
        { "play_or_stop", [] (ActivityFixture&) { return PlayOrStop {}; }, allowed, exportRunning },
        { "seek", [] (ActivityFixture&) { return Seek { 1000 }; }, pass, allowed },
        { "set_master_volume", [] (ActivityFixture&) { return SongIntent { Edit { SetMasterVolume { -3.0 } } }; }, pass, allowed },
        { "undo", [] (ActivityFixture&) { return Undo {}; }, pass, allowed },
        { "redo", [] (ActivityFixture&) { return Redo {}; }, pass, allowed },
        { "add_insert", [] (ActivityFixture& a) { return AddInsert { a.guitarId(), "aufx", "test", "ovdb" }; }, pass, allowed },
        { "preview_mix", [] (ActivityFixture&) { return PreviewMix { SetMasterVolume { -3.0 } }; }, pass, allowed },
        { "set_pre_roll", [] (ActivityFixture&) { return SetPreRoll { song::PreRoll::twoSeconds }; }, pass, allowed },
        { "set_armed", [] (ActivityFixture& a) { return SetArmed { a.guitarId(), false }; }, pass, allowed },
        { "set_input_channels", [] (ActivityFixture& a) { return SetInputChannels { a.guitarId(), { 1 } }; }, pass, allowed },
        { "set_monitoring", [] (ActivityFixture& a) { return SetMonitoring { a.guitarId(), true }; }, pass, allowed },
        { "set_camera_armed", [] (ActivityFixture&) { return SetCameraArmed { true }; }, pass, allowed },
        { "set_camera_device", [] (ActivityFixture&) { return SetCameraDevice { "camera-usb" }; }, pass, allowed },
        { "import_files",
          [] (ActivityFixture& a) { return ImportFiles { { core::AbsolutePath { a.f.fixture->writeSineFile ("Drums.wav", 1.0, 110.0).getFullPathName().toStdString() } } }; },
          pass,
          exportRunning },
        { "export", [] (ActivityFixture&) { return Export {}; }, pass, allowed },
        { "start_separation", [] (ActivityFixture& a) { return StartSeparation { a.bandMixId() }; }, pass, exportRunning },
        { "set_practice_speed", [] (ActivityFixture&) { return SetPracticeSpeed { 80 }; }, pass, allowed },
        { "set_focus_track", [] (ActivityFixture& a) { return SongIntent { SongSetting { SetFocusTrack { a.guitarId() } } }; }, pass, allowed },
        { "set_transcribe_mute", [] (ActivityFixture& a) { return SongIntent { SongSetting { SetTranscribeMute { a.guitarId(), true } } }; }, pass, allowed },
        { "set_transcribe_solo", [] (ActivityFixture& a) { return SongIntent { SongSetting { SetTranscribeSolo { a.guitarId(), true } } }; }, pass, allowed },
    };
}

bool isPresented (const nlohmann::json& event)
{
    return event.at ("event") == "ui.notice_shown" || event.at ("event") == "ui.alert_shown";
}

std::vector<nlohmann::json> presented (const ScopedLogCapture& capture)
{
    std::vector<nlohmann::json> found;
    for (const auto& event : capture.events())
        if (isPresented (event))
            found.push_back (event);
    return found;
}
} // namespace

TEST_CASE ("every gesture against every activity is refused with the activity's code, or allowed", "[ui][activity]")
{
    const auto cases = gestureCases();
    const auto index = GENERATE_COPY (Catch::Generators::range<std::size_t> (0, cases.size()));
    const auto running = GENERATE (Started::nothing, Started::pass, Started::separation, Started::conversion, Started::exporting);
    const auto& gesture = cases[index];
    INFO ("gesture " << gesture.name << ", running " << (int) running);

    ActivityFixture a { running };
    const auto before = a.f.song();
    const auto expected = running == Started::nothing   ? allowed
                        : running == Started::pass      ? gesture.duringPass
                        : running == Started::exporting ? gesture.duringExport
                                                        : runDialog;
    CHECK (outcomeOf (a.f.window->run (gesture.intent (a), Origin::control)) == expected);
    if (expected != allowed)
        CHECK (a.f.song() == before); // a refused gesture changes nothing
}

TEST_CASE ("a refusal names its gesture", "[ui][activity]")
{
    ActivityFixture a { Started::pass };
    const auto refused = a.f.window->run (SongIntent { intent::Undo {} }, Origin::control);
    REQUIRE_FALSE (refused.has_value());
    CHECK (refused.error().code == "ui.stop_recording_first");
    CHECK (refused.error().fields == core::Fields { { "gesture", "undo" } });
    CHECK (isRefusal (refused.error()));
}

TEST_CASE ("only one activity runs: starting another one is refused while it runs, and allowed once it ends", "[ui][activity]")
{
    ActivityFixture a { Started::exporting };
    CHECK (a.f.window->run (SongIntent { intent::StartSeparation { a.bandMixId() } }, Origin::control).error().code == "ui.export_running");
    CHECK (a.f.window->run (SongIntent { intent::Record {} }, Origin::control).error().code == "ui.export_running");

    for (int poll = 0; poll < 200 && a.f.openSong->isExporting(); ++poll)
        a.f.runBlocks (1);
    REQUIRE_FALSE (a.f.openSong->isExporting());
    CHECK (a.f.window->run (SongIntent { intent::Record {} }, Origin::control).has_value());
    CHECK (a.f.openSong->isRecording());
    CHECK_FALSE (a.f.openSong->isSeparating());
}

TEST_CASE ("starting an activity while another runs does nothing and logs the broken invariant", "[ui][activity]")
{
    ActivityFixture a { Started::exporting };
    core::ScopedLogCapture capture;

    CHECK (a.f.openSong->startSeparation (a.bandMixId()).has_value());

    CHECK (a.f.openSong->isExporting());
    CHECK_FALSE (a.f.openSong->isSeparating());
    CHECK (firstEvent (capture, "ui.activity_already_running").at ("kind") == "export");
}

TEST_CASE ("the window presents no Notice or Alert for a gesture a Pass, a Separation or a conversion refuses", "[ui][activity]")
{
    const auto running = GENERATE (Started::pass, Started::separation, Started::conversion);
    // Under a Separation's or a conversion's modal dialog the window refuses first (ui/Availability.h).
    const auto code = running == Started::pass ? pass : std::string ("ui.run_dialog_open");
    ActivityFixture a { running };
    auto& window = *a.f.window;
    int pluginMenusShown = 0;
    int cameraAccessAsked = 0;
    window.listInsertPlugins = [] { return std::vector { testPlugin }; };
    window.chooseInsertPlugin = [&pluginMenusShown] (const std::vector<overdub::plugin::AvailableAudioUnit>&, std::function<void (std::optional<std::size_t>)> done) {
        ++pluginMenusShown;
        done (0);
    };
    window.askCameraAccess = [&cameraAccessAsked] (std::function<void (bool)> done) {
        ++cameraAccessAsked;
        done (true);
    };
    const auto before = a.f.song();
    const auto guitarId = a.guitarId();

    ScopedLogCapture capture;
    if (running == Started::pass)
    {
        window.run (SongIntent { intent::Undo {} });
        window.run (SongIntent { intent::Redo {} });
    }
    auto& view = *window.songView();
    const auto send = [&view] (const SongIntent& intent) { view.recordPage.onIntent (intent); };
    send (SongIntent { intent::Edit { intent::SetTrackMute { guitarId, true } } });
    send (SongIntent { intent::SetArmed { guitarId, false } });
    send (SongIntent { intent::SetInputChannels { guitarId, { 1 } } });
    send (SongIntent { intent::SetMonitoring { guitarId, true } });
    send (SongIntent { intent::SetPreRoll { song::PreRoll::twoSeconds } });
    send (SongIntent { intent::SetCameraArmed { true } });
    send (SongIntent { intent::SetCameraDevice { "camera-usb" } });
    send (SongIntent { intent::AddTrack {} });
    send (SongIntent { intent::MoveTrack { guitarId, 0 } });
    view.recordPage.onAddInsert (guitarId);
    send (SongIntent { intent::Edit { intent::SetInsertBypassed { "no-such-slot", true } } });
    send (SongIntent { intent::PreviewMix { intent::SetMasterVolume { -3.0 } } });

    CHECK (presented (capture).empty());
    CHECK (a.f.alertsShown.empty());
    CHECK (pluginMenusShown == 0); // no dialog opens for a gesture refused anyway
    CHECK (cameraAccessAsked == 0);
    CHECK (a.f.song() == before);
    const auto refusal = firstEvent (capture, code);
    CHECK (refusal.at ("lvl") == "debug");
}

TEST_CASE ("the window still presents a refused record as a Notice while an export runs", "[ui][activity]")
{
    ActivityFixture a { Started::exporting };
    ScopedLogCapture capture;
    a.f.window->run (SongIntent { intent::Record {} });

    const auto shown = presented (capture);
    REQUIRE (shown.size() == 1);
    CHECK (shown.front().at ("event") == "ui.notice_shown");
    CHECK (shown.front().at ("code") == "ui.export_running");
    CHECK_FALSE (a.f.openSong->isRecording());
}
