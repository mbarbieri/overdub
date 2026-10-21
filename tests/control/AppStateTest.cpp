#include "control/AppState.h"

#include "core/Described.h"
#include "core/Log.h"
#include "song/SongJson.h"
#include "ui/WorkspaceFixture.h"

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

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

// Records a Pass on the armed Guitar and lets the window's poll finish it.
void recordPass (WorkspaceFixture& f)
{
    REQUIRE (f.content.run (ui::SongIntent { Record {} }).has_value());
    f.runBlocks (20);
    f.content.run (ui::SongIntent { PlayOrStop {} });
    f.runBlocks (2);
    REQUIRE_FALSE (f.song().isRecording());
    REQUIRE (f.song().song().passes.size() == 1);
}

// A Song with a Backing Track and a recorded Pass, its Band Mix Clip selected, a Notice showing.
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
    // The table agrees with the handler: undo is refused with that code.
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

    f.audio().run (4); // the engine holds new peaks the window's poll has not taken yet
    const auto first = stateOf (f, { "meters" });
    const auto second = stateOf (f, { "meters" });

    CHECK (first == second);
    CHECK (first["meters"]["master"] == shownBefore);
    CHECK (json (f.content.songView()->shownMeters().master) == json (shownBefore));
    f.content.tick(); // the poll still finds the peaks the reads left
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
    CHECK (f.workspace->isIdle()); // the Workspace's own check ignores an export
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
