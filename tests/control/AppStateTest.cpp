#include "control/AppState.h"

#include "control/ControllerHarness.h"
#include "control/IntentOps.h"
#include "core/Described.h"
#include "core/Log.h"
#include "song/FakeSeparationHelper.h"
#include "song/SongJson.h"
#include "ui/UserMessages.h"
#include "ui/WorkspaceFixture.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <nlohmann/json.hpp>

#include <chrono>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

using namespace overdub;
using namespace overdub::ui::intent;
using nlohmann::json;
using overdub::control::appState;
using overdub::ui::testing::eventsNamed;
using overdub::ui::testing::WorkspaceFixture;

namespace
{
std::set<std::string> keysOf (const json& object)
{
    std::set<std::string> keys;
    for (const auto& [key, value] : object.items())
        keys.insert (key);
    return keys;
}

json stateOf (WorkspaceFixture& f, std::vector<std::string_view> parts)
{
    return appState (f.content, *f.workspace, parts);
}

json activityOf (WorkspaceFixture& f)
{
    return stateOf (f, { "activity" })["activity"];
}

json finishedActivity (std::string_view kind, int counter, json errorCode)
{
    return { { "kind", "idle" }, { "progress", 0.0 }, { "counter", counter }, { "last_finish", { { "kind", kind }, { "counter", counter }, { "error_code", std::move (errorCode) } } } };
}

void run (WorkspaceFixture& f, const ui::Intent& intent)
{
    REQUIRE (f.content.run (intent, ui::Origin::control).has_value());
}

// A Pass from the playhead, stopped after 20 blocks; it must be added to the Song.
void recordPass (WorkspaceFixture& f)
{
    const auto passesBefore = f.song().song().passes.size();
    run (f, ui::SongIntent { Record {} });
    f.runBlocks (20);
    run (f, ui::SongIntent { PlayOrStop {} });
    f.runBlocks (2);
    REQUIRE_FALSE (f.song().isRecording());
    REQUIRE (f.song().song().passes.size() == passesBefore + 1);
}

// Ends a running Pass the way the user does, so no Pass is left for teardown to abandon.
void stopPass (WorkspaceFixture& f)
{
    if (! f.song().isRecording())
        return;
    f.content.run (ui::SongIntent { PlayOrStop {} });
    f.runBlocks (2);
    REQUIRE_FALSE (f.song().isRecording());
}

// A Song with one recorded Pass, the Band Mix Clip selected and a Notice shown.
struct SongWithPassSelectionAndNotice
{
    WorkspaceFixture f;
    song::Id bandClip;

    SongWithPassSelectionAndNotice()
    {
        f.launch();
        f.openSong();
        recordPass (f);
        bandClip = f.song().song().tracks.front().clips.front().id;
        run (f, ui::SongIntent { SelectClip { bandClip } });
        f.content.showNotice ("ui.no_audio_device");
    }
};
} // namespace

TEST_CASE ("the app state has every section, each with its keys, over a Song with a Pass, a selection and a Notice", "[control][app_state]")
{
    SongWithPassSelectionAndNotice fixture;
    auto& f = fixture.f;
    const auto state = appState (f.content, *f.workspace);

    CHECK (keysOf (state) == std::set<std::string> { "song", "history", "transport", "activity", "idle", "view", "window", "messages", "audio", "calibration", "camera", "inserts", "monitoring", "meters", "refused" });

    CHECK (state["song"]["path"] == f.song().package().location().getFullPathName().toStdString());
    CHECK (state["song"]["song"] == song::songToJson (f.song().song()));
    CHECK (state["song"]["song"]["passes"].size() == 1);

    CHECK (state["history"] == json { { "can_undo", true }, { "undo_label", "Record Pass 1" }, { "can_redo", false }, { "redo_label", "" } });

    CHECK (keysOf (state["transport"]) == std::set<std::string> { "playing", "position", "pre_roll_record_point", "running_pass_start", "running_pass_number", "saving_pass" });
    CHECK (state["transport"]["playing"] == false);
    CHECK (state["transport"]["running_pass_number"] == 0);
    CHECK (state["transport"]["pre_roll_record_point"] == nullptr);

    CHECK (state["activity"] == finishedActivity ("pass", 1, nullptr));
    CHECK (state["idle"] == true);

    const auto& view = state["view"];
    CHECK (view["mode"] == "record");
    CHECK (view["selected_clip"] == fixture.bandClip);
    CHECK (view["selected_video_take"] == nullptr);
    CHECK (keysOf (view["timeline"]) == std::set<std::string> { "first_sample", "samples_per_pixel", "vertical_scroll", "take_lanes_unfolded" });
    CHECK (view["timeline"]["take_lanes_unfolded"] == json::array());
    CHECK (view["mixer_visible"] == true);
    CHECK (view["passes_panel_open"] == false);
    CHECK (view["export_sheet_shown"] == false);
    CHECK (view["export_form"]["format"] == "wav");
    CHECK (view["export_form"]["length"] == "trim_to_video_take");
    CHECK (view["export_form"]["video_take"] == nullptr);

    CHECK (state["window"] == json { { "video_preview_shown", false }, { "run_dialog", { { "showing", false }, { "title", "" }, { "status", "" }, { "progress", 0.0 } } } });

    CHECK (state["messages"]["notice"]["code"] == "ui.no_audio_device");
    CHECK (state["messages"]["notice"]["text"] == f.content.noticeLine.text().toStdString());
    CHECK (state["messages"]["alerts_shown"] == json::array());
    CHECK (state["messages"]["alerts_deferred"] == json::array());

    CHECK (state["audio"] == json { { "device_type", "Fake" }, { "input_device", "Scarlett 2i2 USB" }, { "output_device", "Scarlett 2i2 USB" }, { "sample_rate", engine::testing::testSampleRate }, { "buffer_size", ui::testing::liveBlockSize }, { "inputs", { "In 1", "In 2" } }, { "outputs", 2 }, { "audio_problem", nullptr } });

    CHECK (keysOf (state["calibration"]) == std::set<std::string> { "device_open", "device_text", "sample_rate", "latency_offset_samples", "camera_id", "camera_name", "camera_offset_ms", "latency_run", "clap_run" });
    CHECK (state["calibration"]["device_open"] == true);
    CHECK (state["calibration"]["latency_run"] == nullptr);

    CHECK (state["camera"]["armed"] == false);
    CHECK (state["camera"]["chosen"] == nullptr);
    CHECK (state["inserts"] == json::object());
    CHECK (state["monitoring"] == json::array());
    CHECK (keysOf (state["meters"]) == std::set<std::string> { "tracks", "inputs", "master" });

    CHECK (state["refused"].size() == core::operationNames<ui::Intent>().size());
    CHECK (state["refused"]["add_track"] == nullptr);
    CHECK (state["refused"]["new_song_at"] == nullptr);
}

TEST_CASE ("the app state with no Song open has null Song sections and the mode", "[control][app_state]")
{
    WorkspaceFixture f;
    f.launch();

    const auto state = appState (f.content, *f.workspace);

    CHECK (state["song"] == nullptr);
    CHECK (state["history"] == nullptr);
    CHECK (state["transport"] == nullptr);
    CHECK (state["view"] == json { { "mode", "record" } });
    CHECK (state["activity"] == json { { "kind", "idle" }, { "progress", 0.0 }, { "counter", 0 }, { "last_finish", nullptr } });
    CHECK (state["refused"]["add_track"] == nullptr);
}

TEST_CASE ("parts select the sections of the app state", "[control][app_state]")
{
    WorkspaceFixture f;
    f.launch();

    CHECK (keysOf (stateOf (f, { "song", "transport" })) == std::set<std::string> { "song", "transport" });
    CHECK (stateOf (f, { "idle" }) == json { { "idle", true } });
}

TEST_CASE ("during a Pass the refused section names each refusal and the transport the running Pass", "[control][app_state]")
{
    WorkspaceFixture f;
    f.launch();
    f.openSong();
    run (f, ui::SongIntent { Record {} });
    f.runBlocks (4);

    const auto state = stateOf (f, { "refused", "transport", "activity", "idle" });

    CHECK (state["refused"]["undo"] == "ui.stop_recording_first");
    CHECK (state["refused"]["set_clip_gain"] == "ui.stop_recording_first");
    CHECK (state["refused"]["record"] == nullptr);
    CHECK (state["refused"]["rewind"] == nullptr);
    CHECK (state["refused"]["new_song_at"] == "ui.stop_recording_first");
    CHECK (state["refused"]["quit"] == nullptr);
    CHECK (state["refused"]["show_mode"] == nullptr);
    CHECK (state["refused"]["show_export"] == "ui.stop_recording_first");
    CHECK (state["transport"]["running_pass_number"] == 1);
    CHECK (state["transport"]["playing"] == true);
    CHECK (state["activity"]["kind"] == "pass");
    CHECK (state["activity"]["counter"] == 1);
    CHECK (state["idle"] == false);
    const auto undone = f.content.run (ui::SongIntent { Undo {} }, ui::Origin::control);
    REQUIRE_FALSE (undone.has_value());
    CHECK (undone.error().code == "ui.stop_recording_first");

    stopPass (f);
}

TEST_CASE ("reading the app state twice never changes the meters the UI shows", "[control][app_state]")
{
    WorkspaceFixture f;
    f.launch();
    f.openSong();
    run (f, ui::SongIntent { PlayOrStop {} });
    f.runBlocks (4);
    const auto shownBefore = f.content.songView()->shownMeters().master;
    REQUIRE (shownBefore > 0.0f);

    f.audio().run (4);
    const auto first = stateOf (f, { "meters" });
    const auto second = stateOf (f, { "meters" });

    CHECK (first == second);
    CHECK (first["meters"]["master"] == shownBefore);
    CHECK (json (f.content.songView()->shownMeters().master) == json (shownBefore));
    f.content.tick();
    CHECK (f.content.songView()->shownMeters().master > 0.0f);
}

TEST_CASE ("during an export the app is not idle, its activity is the export, and convert_sample_rate is refused as it runs", "[control][app_state]")
{
    WorkspaceFixture f;
    f.launch();
    f.openSong();
    run (f, ui::SongIntent { SetExportDestination { core::AbsolutePath { f.folder.getChildFile ("Mix.wav").getFullPathName().toStdString() } } });

    run (f, ui::SongIntent { Export {} });

    REQUIRE (f.song().isExporting());
    const auto state = stateOf (f, { "activity", "idle", "refused" });
    CHECK (state["idle"] == false);
    CHECK (state["activity"]["kind"] == "export");
    CHECK (state["activity"]["counter"] == 1);
    CHECK (state["refused"]["record"] == "ui.export_running");
    CHECK (state["refused"]["undo"] == nullptr);
    CHECK (state["refused"]["convert_sample_rate"] == "ui.export_running");
    CHECK (f.workspace->nothingRunsButAnExport());

    const auto converted = f.run (ui::AppIntent { ConvertSampleRate { 44100 } });
    REQUIRE_FALSE (converted.has_value());
    CHECK (converted.error().code == "ui.export_running");
    CHECK_FALSE (f.song().isConverting());
}

//==============================================================================
// The refused section and the handlers (issue #167): every op the section names as refused,
// run through MainComponent::run, answers that very code.

namespace
{
// A Song where every op has something to act on: two Passes over the Guitar, an Insert on it, a
// step to redo, the Band Mix Clip selected, Edit mode and an export destination.
struct SongForEveryOp
{
    WorkspaceFixture f;
    song::Id bandTrack;
    song::Id bandClip;
    song::Id guitarId;
    song::Id firstPassId;
    song::Id firstTakeId;
    song::SampleCount firstTakeOrigin = 0;
    song::Id slotId;

    SongForEveryOp()
    {
        f.launch();
        f.openSong();
        const plugin::AvailableAudioUnit delay { { "aufx", "dely", "appl" }, "AUDelay", "Apple", "1.0" };
        f.content.listInsertPlugins = [delay] { return std::vector<plugin::AvailableAudioUnit> { delay }; };
        recordPass (f);
        run (f, ui::SongIntent { Seek { 0 } });
        recordPass (f);
        const auto& song = f.song().song();
        bandTrack = song.tracks.at (0).id;
        bandClip = song.tracks.at (0).clips.front().id;
        guitarId = song.tracks.at (1).id;
        firstPassId = song.passes.front().id;
        const auto& firstTake = *std::ranges::find (song.takes, firstPassId, &song::Take::passId);
        firstTakeId = firstTake.id;
        firstTakeOrigin = firstTake.origin;
        slotId = *f.content.run (ui::SongIntent { AddInsert { guitarId, "aufx", "dely", "appl" } }, ui::Origin::control)->createdId;
        run (f, ui::SongIntent { AddTrack {} });
        run (f, ui::SongIntent { Undo {} });
        run (f, ui::SongIntent { SetExportDestination { pathIn ("Mix.wav") } });
        run (f, ui::SongIntent { SelectClip { bandClip } });
        run (f, ui::WindowIntent { ShowMode { ui::Mode::edit } });
    }

    core::AbsolutePath pathIn (const std::string& name) const { return core::AbsolutePath { f.folder.getChildFile (name).getFullPathName().toStdString() }; }

    // Each op's request fields, valid for this Song.
    std::map<std::string, json> requests() const
    {
        const auto path = [this] (const std::string& name) { return pathIn (name).value; };
        return {
            { "new_song_at", { { "path", path ("New.overdub") } } },
            { "open_song_at", { { "path", path ("Other.overdub") } } },
            { "duplicate_song_to", { { "path", path ("Copy.overdub") } } },
            { "convert_sample_rate", { { "rate", 44100 } } },
            { "show_settings", { { "shown", true } } },
            { "apply_device_settings", { { "device_type", "Fake" }, { "input_device", "Scarlett 2i2 USB" }, { "output_device", "Scarlett 2i2 USB" }, { "sample_rate", engine::testing::testSampleRate }, { "buffer_size", ui::testing::liveBlockSize } } },
            { "measure_latency", json::object() },
            { "save_latency_calibration", { { "offset_samples", 10 } } },
            { "measure_clap_pass", json::object() },
            { "save_camera_calibration", { { "camera_id", "camera" }, { "output_device", "Scarlett 2i2 USB" }, { "offset_ms", 1.0 } } },
            { "move_clip", { { "clip_id", bandClip }, { "start", 100 } } },
            { "trim_clip_start", { { "clip_id", bandClip }, { "start", 100 } } },
            { "trim_clip_end", { { "clip_id", bandClip }, { "end", 1000 } } },
            { "set_clip_fade_in", { { "clip_id", bandClip }, { "length", 100 } } },
            { "set_clip_fade_out", { { "clip_id", bandClip }, { "length", 100 } } },
            { "set_clip_gain", { { "clip_id", bandClip }, { "gain_db", -3.0 } } },
            { "bring_back_pass", { { "pass_id", firstPassId } } },
            { "set_track_volume", { { "track_id", guitarId }, { "volume_db", -3.0 } } },
            { "set_track_pan", { { "track_id", guitarId }, { "pan", 0.5 } } },
            { "set_master_volume", { { "volume_db", -3.0 } } },
            { "set_track_mute", { { "track_id", guitarId }, { "mute", true } } },
            { "set_track_solo", { { "track_id", guitarId }, { "solo", true } } },
            { "remove_insert", { { "slot_id", slotId } } },
            { "move_insert", { { "slot_id", slotId }, { "to_index", 0 } } },
            { "set_insert_bypassed", { { "slot_id", slotId }, { "bypassed", true } } },
            { "record", json::object() },
            { "play_or_stop", json::object() },
            { "undo", json::object() },
            { "redo", json::object() },
            { "seek", { { "sample", 0 } } },
            { "jump_to_pass", { { "pass_id", firstPassId } } },
            { "nudge", { { "direction", "forward" } } },
            { "add_track", json::object() },
            { "import_files", { { "files", { path ("Drop.wav") } } } },
            { "delete_track", { { "track_id", guitarId } } },
            { "move_track", { { "track_id", guitarId }, { "to_index", 0 } } },
            { "set_armed", { { "track_id", guitarId }, { "armed", false } } },
            { "set_input_channels", { { "track_id", guitarId }, { "channels", { 0 } } } },
            { "set_monitoring", { { "track_id", guitarId }, { "on", true } } },
            { "start_separation", { { "track_id", bandTrack } } },
            { "set_pre_roll", { { "pre_roll", "one_second" } } },
            { "set_camera_armed", { { "armed", true } } },
            { "set_camera_device", { { "camera_id", "camera" } } },
            { "split_selected_clip", json::object() },
            { "delete_selection", json::object() },
            { "take_lane_click", { { "take_id", firstTakeId }, { "at", firstTakeOrigin + 10 } } },
            { "preview_mix", { { "edit", { { "op", "set_master_volume" }, { "volume_db", -3.0 } } } } },
            { "add_insert", { { "track_id", guitarId }, { "type", "aufx" }, { "subtype", "dely" }, { "manufacturer", "appl" } } },
            { "export", json::object() },
            { "show_export", { { "shown", true } } },
        };
    }

    // Runs every op the refused section names as refused now, each answering with that code.
    // Returns how many ran.
    int runEveryRefusedOp()
    {
        const auto refused = stateOf (f, { "refused" })["refused"];
        const auto requestFor = requests();
        int ran = 0;
        for (const auto& [op, code] : refused.items())
        {
            if (code.is_null())
                continue;
            INFO (op << " is refused with " << code << " in the app state");
            const auto request = requestFor.find (op);
            REQUIRE (request != requestFor.end());
            auto object = request->second;
            object["op"] = op;
            const auto intent = core::parseOperation<ui::Intent> (object, "control.unknown_op", "control.bad_field");
            REQUIRE (intent.has_value());

            const auto result = f.content.run (*intent, ui::Origin::control);

            REQUIRE_FALSE (result.has_value());
            CHECK (result.error().code == code.get<std::string>());
            ++ran;
        }
        return ran;
    }
};
} // namespace

TEST_CASE ("during a Pass every op the refused section names answers that code when run", "[control][app_state][refused]")
{
    SongForEveryOp every;
    auto& f = every.f;
    run (f, ui::SongIntent { Record {} });
    f.runBlocks (4);
    const auto songBefore = f.song().song();

    const auto ran = every.runEveryRefusedOp();

    CHECK (ran > 40);
    CHECK (f.song().isRecording());
    CHECK (f.opened.size() == 1);
    CHECK (f.song().song() == songBefore);
    stopPass (f);
}

TEST_CASE ("during an export every op the refused section names answers that code when run", "[control][app_state][refused]")
{
    SongForEveryOp every;
    auto& f = every.f;
    run (f, ui::SongIntent { Export {} });
    REQUIRE (f.song().isExporting());
    const auto songBefore = f.song().song();

    const auto ran = every.runEveryRefusedOp();

    CHECK (ran >= 6);
    CHECK (f.song().isExporting());
    CHECK (f.song().song() == songBefore);
}

TEST_CASE ("during a latency calibration every op the refused section names answers that code when run, and undo still runs", "[control][app_state][refused][calibration]")
{
    SongForEveryOp every;
    auto& f = every.f;
    f.runBlocks (1);
    run (f, ui::AppIntent { MeasureLatency {} });
    const auto refused = stateOf (f, { "refused" })["refused"];

    const auto ran = every.runEveryRefusedOp();

    CHECK (ran == 15); // the App group but quit, record, play_or_stop, set_monitoring, export and show_export
    CHECK (refused["record"] == "ui.calibration_running");
    CHECK (refused["undo"] == nullptr);
    CHECK (f.content.run (ui::SongIntent { Undo {} }, ui::Origin::control).has_value());
}

//==============================================================================
// Controls and the refused section (issue #195): a control shows its Intent unavailable exactly
// when run refuses it.

namespace
{
enum class Situation
{
    nothing,
    pass,
    exportSong,
    calibration
};

void start (WorkspaceFixture& f, Situation situation)
{
    switch (situation)
    {
        case Situation::nothing: break;
        case Situation::pass:
            run (f, ui::SongIntent { Record {} });
            f.runBlocks (4);
            break;
        case Situation::exportSong:
            run (f, ui::SongIntent { SetExportDestination { core::AbsolutePath { f.folder.getChildFile ("Mix.wav").getFullPathName().toStdString() } } });
            run (f, ui::SongIntent { Export {} });
            break;
        case Situation::calibration:
            f.runBlocks (1);
            run (f, ui::AppIntent { MeasureLatency {} });
            break;
    }
}
} // namespace

TEST_CASE ("the top bar's Undo and Export... and the Track menu's Separate are enabled exactly when run would not refuse them", "[control][app_state][refused]")
{
    const auto situation = GENERATE (Situation::nothing, Situation::pass, Situation::exportSong, Situation::calibration);
    WorkspaceFixture f;
    f.launch();
    f.openSong();
    run (f, ui::SongIntent { AddTrack {} });
    std::vector<ui::TrackMenuItem> menuShown;
    f.content.chooseTrackMenuItem = [&menuShown] (const std::vector<ui::TrackMenuItem>& items, std::function<void (std::optional<std::size_t>)> done) {
        menuShown = items;
        done (std::nullopt);
    };
    start (f, situation);
    f.content.refresh();
    const auto refused = stateOf (f, { "refused" })["refused"];

    f.content.songView()->recordPage.onTrackMenu (f.song().song().tracks.front().id);

    // What the user may do in each situation; the refused section, the controls and run agree.
    const auto mayUndo = situation != Situation::pass;
    const auto maySeparate = situation == Situation::nothing || situation == Situation::calibration;
    REQUIRE (menuShown.size() == 1);
    CHECK (menuShown[0].enabled == maySeparate);
    CHECK (refused["start_separation"].is_null() == maySeparate);
    CHECK (f.content.topBar.undoButton.isEnabled() == mayUndo);
    CHECK (refused["undo"].is_null() == mayUndo);
    CHECK (f.content.run (ui::SongIntent { Undo {} }, ui::Origin::control).has_value() == mayUndo);
    // Export... (issue #200) waits until nothing runs.
    const auto mayExport = situation == Situation::nothing;
    CHECK (f.content.topBar.exportButton.isEnabled() == mayExport);
    CHECK (refused["show_export"].is_null() == mayExport);
    CHECK (f.content.run (ui::SongIntent { ShowExport { true } }, ui::Origin::control).has_value() == mayExport);
    stopPass (f);
}

//==============================================================================
// The activity log: counter and last_finish

TEST_CASE ("the activity counter grows with each start and last_finish records the error code", "[control][app_state][activity]")
{
    WorkspaceFixture f;
    f.launch();
    f.openSong();
    recordPass (f);
    CHECK (activityOf (f)["counter"] == 1);

    REQUIRE (f.run (ui::AppIntent { MeasureClapPass {} }).has_value());

    CHECK (activityOf (f) == finishedActivity ("calibration_clap", 2, "capture.clap_no_video_take"));
}

TEST_CASE ("a Pass that stood but raised a Notice finishes without an error code", "[control][app_state][activity]")
{
    WorkspaceFixture f;
    f.launch();
    f.openSong();
    run (f, ui::SongIntent { SetCameraArmed { true } });
    f.camera.beginError = core::Error { "capture.movie_write_failed", "the movie could not be written", {} };
    core::ScopedLogCapture capture;

    recordPass (f);

    CHECK (f.song().song().videoTakes.empty());
    CHECK (ui::testing::contains (ui::testing::messageCodes (capture), "capture.movie_write_failed"));
    CHECK (activityOf (f) == finishedActivity ("pass", 1, nullptr));
}

TEST_CASE ("a Pass that was not added finishes with its Error's code", "[control][app_state][activity]")
{
    WorkspaceFixture f;
    f.launch();
    f.openSong();
    run (f, ui::SongIntent { SetPreRoll { song::PreRoll::oneSecond } });
    run (f, ui::SongIntent { Seek { 2 * engine::testing::testSampleRate } });

    run (f, ui::SongIntent { Record {} });
    f.runBlocks (2); // stopped long before the record point: nothing was captured
    stopPass (f);

    CHECK (f.song().song().passes.empty());
    CHECK (activityOf (f) == finishedActivity ("pass", 1, "capture.pass_empty"));
}

TEST_CASE ("an export that ends is the last finish, without an error code", "[control][app_state][activity][export]")
{
    WorkspaceFixture f;
    f.launch();
    f.openSong();
    run (f, ui::SongIntent { SetExportDestination { core::AbsolutePath { f.folder.getChildFile ("Mix.wav").getFullPathName().toStdString() } } });
    run (f, ui::SongIntent { Export {} });

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds (20);
    while (f.song().isExporting() && std::chrono::steady_clock::now() < deadline)
    {
        f.runBlocks (1);
        std::this_thread::sleep_for (std::chrono::milliseconds (2));
    }

    REQUIRE_FALSE (f.song().isExporting());
    REQUIRE (f.song().lastExportedFile().has_value());
    CHECK (activityOf (f) == finishedActivity ("export", 1, nullptr));
}

TEST_CASE ("a cancelled Separation is the last finish with song.separation_cancelled", "[control][app_state][activity][separation]")
{
    WorkspaceFixture f;
    f.launch();
    f.openSong();
    auto log = std::make_shared<song::testing::FakeHelperLog>();
    f.song().setSeparationTools (song::SeparationTools {
        .model = song::testing::writeStandInModel (f.folder),
        .startHelper = song::testing::fakeSeparationHelper ({}, log),
        .workFolder = f.folder.getChildFile ("work"),
    });
    run (f, ui::SongIntent { StartSeparation { f.song().song().tracks.front().id } });
    CHECK (activityOf (f)["kind"] == "separation");
    CHECK (stateOf (f, { "idle" })["idle"] == false);

    run (f, ui::SongIntent { CancelRun {} });

    CHECK (activityOf (f) == finishedActivity ("separation", 1, "song.separation_cancelled"));
}

TEST_CASE ("during a Separation the refused section lets through only what the run dialog lets through", "[control][app_state][refused][separation]")
{
    WorkspaceFixture f;
    f.launch();
    f.openSong();
    f.song().setSeparationTools (song::SeparationTools {
        .model = song::testing::writeStandInModel (f.folder),
        .startHelper = song::testing::fakeSeparationHelper ({}, std::make_shared<song::testing::FakeHelperLog>()),
        .workFolder = f.folder.getChildFile ("work"),
    });
    run (f, ui::SongIntent { StartSeparation { f.song().song().tracks.front().id } });

    const auto refused = stateOf (f, { "refused" })["refused"];
    CHECK (refused["cancel_run"] == nullptr);
    CHECK (refused["close_plugin_window"] == nullptr);
    CHECK (refused["undo"] == "ui.run_dialog_open");
    CHECK (refused["seek"] == "ui.run_dialog_open");
    CHECK (refused["show_mode"] == "ui.run_dialog_open");
}

TEST_CASE ("a sample rate conversion that ends is the last finish, without an error code", "[control][app_state][activity][conversion]")
{
    WorkspaceFixture f;
    f.launch();
    f.openSong (44100); // the device runs at 48 kHz only
    run (f, ui::AppIntent { ConvertSampleRate { 48000 } });
    CHECK (activityOf (f)["kind"] == "conversion");

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds (30);
    while (f.song().isConverting() && std::chrono::steady_clock::now() < deadline)
    {
        f.tick();
        std::this_thread::sleep_for (std::chrono::milliseconds (5));
    }

    REQUIRE_FALSE (f.song().isConverting());
    CHECK (f.song().song().sampleRate == 48000);
    CHECK (activityOf (f) == finishedActivity ("conversion", 1, nullptr));
}

TEST_CASE ("a latency run is the calibration_latency activity while it runs, then the last finish", "[control][app_state][activity][calibration]")
{
    WorkspaceFixture f;
    f.launch();
    f.openSong();
    f.runBlocks (1);

    run (f, ui::AppIntent { MeasureLatency {} });

    const auto running = stateOf (f, { "activity", "idle" });
    CHECK (running["activity"]["kind"] == "calibration_latency");
    CHECK (running["activity"]["counter"] == 1);
    CHECK (running["idle"] == false);

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds (30);
    while (! f.workspace->nothingRunsButAnExport() && std::chrono::steady_clock::now() < deadline)
    {
        f.audio().run (8);
        f.workspace->tick();
    }
    REQUIRE (f.workspace->nothingRunsButAnExport());
    CHECK (activityOf (f) == finishedActivity ("calibration_latency", 1, nullptr));
}

TEST_CASE ("the messages section lists the Notice with its action and hide time, each open Alert, and the Alerts a Pass defers", "[control][app_state][messages]")
{
    WorkspaceFixture f;
    f.launch();
    f.openSong();
    const auto messagesOf = [&f] { return stateOf (f, { "messages" })["messages"]; };

    const auto shownAt = juce::Time::currentTimeMillis();
    f.content.showNotice ("ui.no_audio_device");
    auto notice = messagesOf()["notice"];
    CHECK (notice["code"] == "ui.no_audio_device");
    CHECK (notice["tier"] == "notice");
    CHECK (notice["text"] == ui::sentenceFor ("ui.no_audio_device"));
    CHECK (notice["action"] == nullptr);
    const auto firstCounter = notice["counter"].get<int>();
    CHECK (notice["hides_at_ms"].get<juce::int64>() >= shownAt + ui::NoticeLine::visibleForMs);
    CHECK (notice["hides_at_ms"].get<juce::int64>() <= juce::Time::currentTimeMillis() + ui::NoticeLine::visibleForMs);
    CHECK (messagesOf()["notice"]["hides_at_ms"] == notice["hides_at_ms"]);

    f.content.handleError (core::Error { "ui.sample_rate_mismatch", "test", { { "song_rate", 44100 }, { "device_rate", 48000 }, { "rate_offered", false } } });
    notice = messagesOf()["notice"];
    CHECK (notice["code"] == "ui.sample_rate_mismatch");
    CHECK (notice["counter"] == firstCounter + 1);
    CHECK (notice["action"] == json { { "label", "Convert to 48000 Hz" }, { "intent", { { "op", "convert_sample_rate" }, { "rate", 48000 } } } });
    CHECK (notice["hides_at_ms"] == nullptr);

    f.content.handleError (core::Error { "song.import_unreadable", "test", {} });
    const json importAlert { { "code", "song.import_unreadable" }, { "tier", "alert" }, { "text", ui::sentenceFor ("song.import_unreadable") } };
    CHECK (messagesOf()["alerts_shown"] == json::array ({ importAlert }));
    CHECK (messagesOf()["alerts_deferred"] == json::array());

    run (f, ui::SongIntent { Record {} });
    f.runBlocks (4);
    f.content.handleError (core::Error { "capture.take_write_failed", "test", {} });
    const json writeAlert { { "code", "capture.take_write_failed" }, { "tier", "alert" }, { "text", ui::sentenceFor ("capture.take_write_failed") } };
    CHECK (messagesOf()["alerts_shown"] == json::array ({ importAlert }));
    CHECK (messagesOf()["alerts_deferred"] == json::array ({ writeAlert }));

    stopPass (f);
    CHECK (messagesOf()["alerts_shown"] == json::array ({ importAlert, writeAlert }));
    CHECK (messagesOf()["alerts_deferred"] == json::array());
}

//==============================================================================
// Over the socket

namespace
{
struct SocketToWorkspace
{
    WorkspaceFixture f;
    control::Clock::time_point now {};
    std::shared_ptr<control::EventTap> tap = std::make_shared<control::EventTap>();
    std::unique_ptr<control::Controller> controller;
    control::ConnectionId client = 0;

    SocketToWorkspace()
    {
        f.launch();
        f.openSong();
        controller = std::make_unique<control::Controller> (tap, control::intentOps (f.content), control::appStateReader (f.content, *f.workspace));
        client = controller->connect();
    }

    std::vector<json> send (const std::string& request, std::chrono::milliseconds later = {})
    {
        now += later;
        controller->receive (client, request + "\n", now);
        controller->pump (now);
        return control::testing::takeLines (*controller, client);
    }

    std::vector<json> pump (std::chrono::milliseconds later)
    {
        now += later;
        controller->pump (now);
        return control::testing::takeLines (*controller, client);
    }
};
} // namespace

TEST_CASE ("state over the socket answers the sections asked for, and refuses an unknown one", "[control][app_state][socket]")
{
    core::ScopedLogCapture capture;
    SocketToWorkspace socket;
    const auto trackId = socket.send (R"({"id":1,"op":"add_track"})").at (0).at ("created_id").get<std::string>();

    const auto lines = socket.send (R"({"id":2,"op":"state","parts":["history","view"]})");

    REQUIRE (lines.size() == 1);
    CHECK (lines[0]["ok"] == true);
    CHECK (lines[0]["history"] == json { { "can_undo", true }, { "undo_label", "Add Track 1" }, { "can_redo", false }, { "redo_label", "" } });
    CHECK (lines[0]["view"]["mode"] == "record");
    CHECK_FALSE (lines[0].contains ("song"));
    CHECK (song::findTrack (socket.f.song().song(), trackId) != nullptr);

    const auto all = socket.send (R"({"id":3,"op":"state"})");
    REQUIRE (all.size() == 1);
    for (const auto part : control::appStateParts)
        CHECK (all[0].contains (std::string (part)));

    const auto refused = socket.send (R"({"id":4,"op":"state","parts":["song","nope"]})");
    REQUIRE (refused.size() == 1);
    CHECK (refused[0]["error"]["code"] == "control.bad_field");
    CHECK (refused[0]["error"]["fields"]["key"] == "parts");
    const auto logged = eventsNamed (capture, "control.bad_field");
    REQUIRE (logged.size() == 1);
    CHECK (logged[0]["key"] == "parts");
}

TEST_CASE ("an Alert an Intent raised shows in state over the socket until dismiss_alerts closes it; a second dismiss_alerts does nothing", "[control][app_state][socket][messages]")
{
    core::ScopedLogCapture capture;
    SocketToWorkspace socket;
    int closesByWindow = 0;
    socket.f.content.showAlert = [&closesByWindow] (const juce::String&, std::function<void()>) { return [&closesByWindow] { ++closesByWindow; }; };
    const auto notASong = json { { "id", 1 }, { "op", "open_song_at" }, { "path", socket.f.folder.getFullPathName().toStdString() } };

    const auto opened = socket.send (notASong.dump());
    REQUIRE (opened.size() == 1);
    CHECK (opened[0]["error"]["code"] == "song.package_not_found");

    const auto before = socket.send (R"({"id":2,"op":"state","parts":["messages"]})");
    REQUIRE (before.size() == 1);
    CHECK (before[0]["messages"]["alerts_shown"] == json::array ({ { { "code", "song.package_not_found" }, { "tier", "alert" }, { "text", ui::sentenceFor ("song.package_not_found") } } }));

    const auto dismissed = socket.send (R"({"id":3,"op":"dismiss_alerts"})");
    REQUIRE (dismissed.size() == 1);
    CHECK (dismissed[0]["ok"] == true);
    CHECK (closesByWindow == 1);
    const auto logged = eventsNamed (capture, "ui.alert_dismissed");
    REQUIRE (logged.size() == 1);
    CHECK (logged[0]["code"] == "song.package_not_found");
    CHECK (logged[0]["tier"] == "alert");

    const auto after = socket.send (R"({"id":4,"op":"state","parts":["messages"]})");
    REQUIRE (after.size() == 1);
    CHECK (after[0]["messages"]["alerts_shown"] == json::array());

    const auto again = socket.send (R"({"id":5,"op":"dismiss_alerts"})");
    REQUIRE (again.size() == 1);
    CHECK (again[0]["ok"] == true);
    CHECK (closesByWindow == 1);
    CHECK (eventsNamed (capture, "ui.alert_dismissed").size() == 1);
}

TEST_CASE ("a pointer wait over the socket answers once an Intent changes the app state, and times out otherwise", "[control][app_state][socket][wait]")
{
    core::ScopedLogCapture capture;
    SocketToWorkspace socket;

    CHECK (socket.send (R"({"id":1,"op":"wait","pointer":"/view/mode","equals":"edit","timeout_ms":2000})").empty());
    CHECK (socket.pump (std::chrono::milliseconds (30)).empty());

    const auto lines = socket.send (R"({"id":2,"op":"show_mode","mode":"edit"})", std::chrono::milliseconds (30));
    REQUIRE (lines.size() == 2);
    CHECK (lines[0]["id"] == 2);
    CHECK (lines[0]["ok"] == true);
    CHECK (lines[1]["id"] == 1);
    CHECK (lines[1]["pointer"] == "/view/mode");
    CHECK (lines[1]["value"] == "edit");

    CHECK (socket.send (R"({"id":3,"op":"wait","pointer":"/activity/kind","equals":"export","timeout_ms":100})").empty());
    const auto timedOut = socket.pump (std::chrono::milliseconds (100));
    REQUIRE (timedOut.size() == 1);
    CHECK (timedOut[0]["error"]["code"] == "control.wait_timed_out");
    CHECK (timedOut[0]["error"]["fields"] == json { { "pointer", "/activity/kind" }, { "equals", "export" }, { "value", "idle" }, { "timeout_ms", 100 } });
}

TEST_CASE ("a pointer wait into an unknown section is a bad pointer, logged once under that key", "[control][app_state][socket][wait]")
{
    core::ScopedLogCapture capture;
    SocketToWorkspace socket;

    const auto lines = socket.send (R"({"id":1,"op":"wait","pointer":"/nope/kind","equals":"idle"})");

    REQUIRE (lines.size() == 1);
    CHECK (lines[0]["error"]["code"] == "control.bad_field");
    CHECK (lines[0]["error"]["fields"]["key"] == "pointer");
    const auto logged = eventsNamed (capture, "control.bad_field");
    REQUIRE (logged.size() == 1);
    CHECK (logged[0]["key"] == "pointer");
}
