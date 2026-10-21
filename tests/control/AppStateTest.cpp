#include "control/AppState.h"

#include "control/ControllerHarness.h"
#include "control/IntentOps.h"
#include "core/Described.h"
#include "core/Log.h"
#include "song/SongJson.h"
#include "ui/WorkspaceFixture.h"

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include <chrono>
#include <memory>
#include <set>
#include <string>
#include <string_view>
#include <vector>

using namespace overdub;
using namespace overdub::ui::intent;
using nlohmann::json;
using overdub::control::appState;
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

void recordPass (WorkspaceFixture& f)
{
    REQUIRE (f.content.run (ui::SongIntent { Record {} }).has_value());
    f.runBlocks (20);
    f.content.run (ui::SongIntent { PlayOrStop {} });
    f.runBlocks (2);
    REQUIRE_FALSE (f.song().isRecording());
    REQUIRE (f.song().song().passes.size() == 1);
}

struct RichWorkspace
{
    WorkspaceFixture f;
    song::Id bandClip;
    song::Id guitarId;

    RichWorkspace()
    {
        f.launch();
        f.openSong();
        recordPass (f);
        bandClip = f.song().song().tracks.front().clips.front().id;
        guitarId = f.song().song().tracks.at (1).id;
        REQUIRE (f.content.run (ui::SongIntent { SelectClip { bandClip } }).has_value());
        f.content.showNotice ("ui.no_audio_device");
    }
};
} // namespace

TEST_CASE ("the app state has every section, each with its keys, over a Song with a Pass, a selection and a Notice", "[control][app_state]")
{
    RichWorkspace rich;
    auto& f = rich.f;
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

    CHECK (state["activity"] == json { { "kind", "idle" }, { "progress", 0.0 }, { "counter", 1 }, { "last_finish", { { "kind", "pass" }, { "counter", 1 }, { "error_code", nullptr } } } });
    CHECK (state["idle"] == true);

    const auto& view = state["view"];
    CHECK (view["mode"] == "record");
    CHECK (view["selected_clip"] == rich.bandClip);
    CHECK (view["selected_video_take"] == nullptr);
    CHECK (keysOf (view["timeline"]) == std::set<std::string> { "first_sample", "samples_per_pixel", "vertical_scroll", "take_lanes_unfolded" });
    CHECK (view["timeline"]["take_lanes_unfolded"] == json::array());
    CHECK (view["mixer_visible"] == true);
    CHECK (view["passes_panel_open"] == false);
    CHECK (view["export_form"]["format"] == "wav");
    CHECK (view["export_form"]["length"] == "trim_to_video_take");
    CHECK (view["export_form"]["video_take"] == nullptr);

    CHECK (state["window"] == json { { "video_preview_wanted", false }, { "video_preview_shown", false }, { "run_dialog", { { "showing", false }, { "title", "" }, { "status", "" }, { "progress", 0.0 } } } });

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
    REQUIRE (f.content.run (ui::SongIntent { Record {} }).has_value());
    f.runBlocks (4);

    const auto state = stateOf (f, { "refused", "transport", "activity", "idle" });

    CHECK (state["refused"]["undo"] == "ui.stop_recording_first");
    CHECK (state["refused"]["set_clip_gain"] == "ui.stop_recording_first");
    CHECK (state["refused"]["record"] == nullptr);
    CHECK (state["refused"]["rewind"] == nullptr);
    CHECK (state["refused"]["new_song_at"] == "ui.stop_recording_first");
    CHECK (state["refused"]["quit"] == nullptr);
    CHECK (state["refused"]["show_mode"] == nullptr);
    CHECK (state["transport"]["running_pass_number"] == 1);
    CHECK (state["transport"]["playing"] == true);
    CHECK (state["activity"]["kind"] == "pass");
    CHECK (state["activity"]["counter"] == 1);
    CHECK (state["idle"] == false);
    const auto undone = f.content.run (ui::SongIntent { Undo {} }, ui::Origin::control);
    REQUIRE_FALSE (undone.has_value());
    CHECK (undone.error().code == "ui.stop_recording_first");

    f.content.run (ui::SongIntent { PlayOrStop {} });
    f.runBlocks (2);
    REQUIRE_FALSE (f.song().isRecording());
}

TEST_CASE ("reading the app state twice never changes the meters the UI shows", "[control][app_state]")
{
    WorkspaceFixture f;
    f.launch();
    f.openSong();
    REQUIRE (f.content.run (ui::SongIntent { PlayOrStop {} }).has_value());
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

TEST_CASE ("during an export the app is not idle and its activity is the export", "[control][app_state]")
{
    WorkspaceFixture f;
    f.launch();
    f.openSong();
    REQUIRE (f.content.run (ui::SongIntent { SetExportDestination { core::AbsolutePath { f.folder.getChildFile ("Mix.wav").getFullPathName().toStdString() } } }).has_value());

    REQUIRE (f.content.run (ui::SongIntent { Export {} }).has_value());

    REQUIRE (f.song().isExporting());
    const auto state = stateOf (f, { "activity", "idle", "refused" });
    CHECK (state["idle"] == false);
    CHECK (state["activity"]["kind"] == "export");
    CHECK (state["activity"]["counter"] == 1);
    CHECK (state["refused"]["record"] == "ui.export_running");
    CHECK (state["refused"]["undo"] == nullptr);
    CHECK (f.workspace->isIdle());
}

TEST_CASE ("the activity counter grows with each start and last_finish records the error code", "[control][app_state]")
{
    WorkspaceFixture f;
    f.launch();
    f.openSong();
    recordPass (f);
    CHECK (stateOf (f, { "activity" })["activity"]["counter"] == 1);

    REQUIRE (f.run (ui::AppIntent { MeasureClapPass {} }).has_value());

    CHECK (stateOf (f, { "activity" })["activity"] == json { { "kind", "idle" }, { "progress", 0.0 }, { "counter", 2 }, { "last_finish", { { "kind", "calibration_clap" }, { "counter", 2 }, { "error_code", "capture.clap_no_video_take" } } } });
}

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
    overdub::core::ScopedLogCapture capture;
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
}

TEST_CASE ("a pointer wait over the socket answers once an Intent changes the app state, and times out otherwise", "[control][app_state][socket][wait]")
{
    overdub::core::ScopedLogCapture capture;
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
