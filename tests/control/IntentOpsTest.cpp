#include "control/IntentOps.h"

#include "control/ControllerHarness.h"
#include "core/Described.h"
#include "song/Song.h"
#include "ui/MainWindowFixture.h"

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

using namespace overdub;
using namespace overdub::control;
using namespace overdub::control::testing;
using nlohmann::json;

namespace
{
const Clock::time_point start {};

struct SocketToWindow
{
    ui::testing::MainWindowFixture f;
    std::shared_ptr<EventTap> tap = std::make_shared<EventTap>();
    Controller controller { tap, intentOps (*f.window) };
    ConnectionId client = controller.connect();

    std::vector<json> send (const std::string& request)
    {
        controller.receive (client, request + "\n", start);
        controller.pump (start);
        return takeLines (controller, client);
    }

    json request (const std::string& request)
    {
        const auto lines = send (request);
        REQUIRE (lines.size() == 1);
        return lines[0];
    }
};

const json* schemaOf (const json& ops, const std::string& op)
{
    const auto found = std::ranges::find_if (ops, [&op] (const json& schema) { return schema.at ("op") == op; });
    return found == ops.end() ? nullptr : &*found;
}
} // namespace

TEST_CASE ("Intents from the socket change the Song and reply with what they created", "[control][intent_ops]")
{
    SocketToWindow socket;
    const auto tracksBefore = socket.f.song().tracks.size();

    const auto added = socket.request (R"({"id":1,"op":"add_track"})");
    CHECK (added["ok"] == true);
    CHECK (added["awaiting_answer"] == false);
    REQUIRE (added["created_id"].is_string());
    const auto trackId = added["created_id"].get<std::string>();
    REQUIRE (socket.f.song().tracks.size() == tracksBefore + 1);
    const auto* track = song::findTrack (socket.f.song(), trackId);
    REQUIRE (track != nullptr);
    CHECK_FALSE (track->mute);
    const auto trackName = track->name;

    const auto muted = socket.request (R"({"id":2,"op":"set_track_mute","track_id":")" + trackId + R"(","mute":true})");
    CHECK (muted == json { { "id", 2 }, { "ok", true }, { "seq", muted["seq"] }, { "created_id", nullptr }, { "awaiting_answer", false } });
    CHECK (song::findTrack (socket.f.song(), trackId)->mute);
    CHECK (socket.f.openSong->package().undoLabel() == "Mute " + trackName);
}

TEST_CASE ("an Intent's ui.intent_run reaches a subscriber with origin control before its reply", "[control][intent_ops]")
{
    core::ScopedLogCapture capture;
    SocketToWindow socket;
    ProcessTap installed { socket.tap };
    REQUIRE (socket.request (R"({"id":1,"op":"subscribe","events":["ui.intent_run"],"level":"debug"})")["ok"] == true);

    const auto lines = socket.send (R"({"id":2,"op":"show_mode","mode":"edit"})");

    REQUIRE (lines.size() == 2);
    CHECK (lines[0]["event"]["event"] == "ui.intent_run");
    CHECK (lines[0]["event"]["op"] == "show_mode");
    CHECK (lines[0]["event"]["origin"] == "control");
    CHECK (lines[1]["id"] == 2);
    CHECK (lines[1]["ok"] == true);
    CHECK (lines[0]["seq"].get<std::uint64_t>() <= lines[1]["seq"].get<std::uint64_t>());
    CHECK (socket.f.window->mode() == ui::Mode::edit);
}

TEST_CASE ("an Intent the running Pass refuses is an error with refused true, and other handled Errors have refused false", "[control][intent_ops]")
{
    SocketToWindow socket;
    REQUIRE (socket.request (R"({"id":1,"op":"record"})")["ok"] == true);
    REQUIRE (socket.f.openSong->isRecording());
    const auto before = socket.f.song();
    const auto guitarId = socket.f.trackNamed ("Guitar").id;

    const auto refused = socket.request (R"({"id":2,"op":"delete_track","track_id":")" + guitarId + R"("})");

    CHECK (refused["ok"] == false);
    CHECK (refused["error"]["code"] == "ui.stop_recording_first");
    CHECK (refused["error"]["fields"]["refused"] == true);
    CHECK (refused["error"]["fields"].contains ("gesture"));
    CHECK (socket.f.song() == before);

    REQUIRE (socket.request (R"({"id":3,"op":"play_or_stop"})")["ok"] == true);
    socket.f.runBlocks (1);
    REQUIRE_FALSE (socket.f.openSong->isRecording());

    const auto notRefused = socket.request (R"({"id":4,"op":"redo"})");
    CHECK (notRefused["ok"] == false);
    CHECK (notRefused["error"]["code"] == "ui.nothing_to_redo");
    CHECK (notRefused["error"]["fields"]["refused"] == false);
}

TEST_CASE ("a request with a bad field or an unknown op runs nothing and names what was wrong", "[control][intent_ops]")
{
    core::ScopedLogCapture capture;
    SocketToWindow socket;
    const auto before = socket.f.song();
    const auto guitarId = socket.f.trackNamed ("Guitar").id;

    const auto mistyped = socket.request (R"({"id":1,"op":"set_track_mute","track_id":")" + guitarId + R"(","mute":"yes"})");
    CHECK (mistyped["ok"] == false);
    CHECK (mistyped["error"]["code"] == "control.bad_field");
    CHECK (mistyped["error"]["fields"] == json { { "key", "mute" }, { "expected", "boolean" } });

    const auto unknownKey = socket.request (R"({"id":2,"op":"set_track_mute","track_id":")" + guitarId + R"(","mute":true,"volume":3})");
    CHECK (unknownKey["error"]["code"] == "control.bad_field");
    CHECK (unknownKey["error"]["fields"] == json { { "key", "volume" }, { "expected", "no such key" } });

    const auto badEnum = socket.request (R"({"id":3,"op":"show_mode","mode":"mix"})");
    CHECK (badEnum["error"]["code"] == "control.bad_field");
    CHECK (badEnum["error"]["fields"]["key"] == "mode");

    const auto unknown = socket.request (R"({"id":4,"op":"frobnicate"})");
    CHECK (unknown["ok"] == false);
    CHECK (unknown["error"]["code"] == "control.unknown_op");
    CHECK (unknown["error"]["fields"] == json { { "op", "frobnicate" } });

    CHECK (socket.f.song() == before);
    const auto events = capture.events();
    CHECK (std::ranges::none_of (events, [] (const json& event) { return event.at ("event") == "ui.intent_run"; }));
    CHECK (std::ranges::count_if (events, [] (const json& event) { return event.at ("event") == "control.bad_field"; }) == 3);
    CHECK (std::ranges::count_if (events, [] (const json& event) { return event.at ("event") == "control.unknown_op"; }) == 1);
}

TEST_CASE ("ops lists every Intent with its group and the JSON form of each field", "[control][intent_ops]")
{
    SocketToWindow socket;

    const auto reply = socket.request (R"({"id":1,"op":"ops"})");

    REQUIRE (reply["ok"] == true);
    const auto& ops = reply["ops"];
    const auto names = core::operationNames<ui::Intent>();
    REQUIRE (ops.size() == names.size());
    for (std::size_t i = 0; i < names.size(); ++i)
        CHECK (ops[i]["op"] == std::string (names[i]));

    const auto* showMode = schemaOf (ops, "show_mode");
    REQUIRE (showMode != nullptr);
    CHECK (*showMode == json { { "op", "show_mode" }, { "group", "window" }, { "fields", json::array ({ { { "key", "mode" }, { "type", "string" }, { "enum", { "record", "edit", "transcribe" } } } }) } });

    const auto* mute = schemaOf (ops, "set_track_mute");
    REQUIRE (mute != nullptr);
    CHECK (*mute == json { { "op", "set_track_mute" }, { "group", "song" }, { "fields", json::array ({ { { "key", "track_id" }, { "type", "string" } }, { { "key", "mute" }, { "type", "boolean" } } }) } });

    const auto* openSong = schemaOf (ops, "open_song_at");
    REQUIRE (openSong != nullptr);
    CHECK ((*openSong)["group"] == "app");
    CHECK ((*openSong)["fields"] == json::array ({ { { "key", "path" }, { "type", "string" }, { "format", "absolute_path" } } }));

    const auto* import = schemaOf (ops, "import_files");
    REQUIRE (import != nullptr);
    CHECK ((*import)["fields"][0]["items"] == json { { "type", "string" }, { "format", "absolute_path" } });

    const auto* preview = schemaOf (ops, "preview_mix");
    REQUIRE (preview != nullptr);
    const auto& previewEdit = (*preview)["fields"][0];
    CHECK (previewEdit["optional"] == true);
    CHECK (previewEdit["type"] == "object");
    REQUIRE (previewEdit["ops"].size() == 3);
    CHECK (previewEdit["ops"][0]["op"] == "set_track_volume");
}

TEST_CASE ("insert_plugins lists the AU effects on offer with the component add_insert names", "[control][intent_ops]")
{
    SocketToWindow socket;

    SECTION ("none on offer")
    {
        const auto reply = socket.request (R"({"id":1,"op":"insert_plugins"})");
        CHECK (reply == json { { "id", 1 }, { "ok", true }, { "seq", reply["seq"] }, { "plugins", json::array() } });
    }
    SECTION ("two on offer, in the window's order")
    {
        socket.f.window->listInsertPlugins = [] {
            return std::vector<plugin::AvailableAudioUnit> {
                { .component = { "aufx", "dely", "appl" }, .name = "AUDelay", .manufacturer = "Apple", .version = "1.6.0", .isAUv3 = false },
                { .component = { "aufx", "Rvb3", "Acme" }, .name = "Room", .manufacturer = "Acme", .version = "2.0.1", .isAUv3 = true },
            };
        };

        const auto reply = socket.request (R"({"id":1,"op":"insert_plugins"})");

        CHECK (reply["plugins"] == json::array ({
                   { { "type", "aufx" }, { "subtype", "dely" }, { "manufacturer", "appl" }, { "name", "AUDelay" }, { "version", "1.6.0" }, { "is_auv3", false } },
                   { { "type", "aufx" }, { "subtype", "Rvb3" }, { "manufacturer", "Acme" }, { "name", "Room" }, { "version", "2.0.1" }, { "is_auv3", true } },
               }));
    }
}

TEST_CASE ("every op in appOps is answered by intentOps and is not an Intent", "[control][intent_ops]")
{
    SocketToWindow socket;

    for (const auto op : appOps)
    {
        const auto reply = socket.request (json { { "id", 1 }, { "op", op } }.dump());
        CHECK (reply["ok"] == true);
    }
    CHECK (appOps == std::array<std::string_view, 2> { "ops", "insert_plugins" });
}

TEST_CASE ("preview_mix takes a nested edit or null, and a bad nested field is named by its dotted key path", "[control][intent_ops]")
{
    core::ScopedLogCapture capture;
    SocketToWindow socket;
    const auto before = socket.f.song();
    const auto guitarId = socket.f.trackNamed ("Guitar").id;

    SECTION ("a valid nested edit")
    {
        const auto reply = socket.request (R"({"id":1,"op":"preview_mix","edit":{"op":"set_track_volume","track_id":")" + guitarId + R"(","volume_db":-6.0}})");
        CHECK (reply == json { { "id", 1 }, { "ok", true }, { "seq", reply["seq"] }, { "created_id", nullptr }, { "awaiting_answer", false } });
    }
    SECTION ("null: hear the Song as it is")
    {
        const auto reply = socket.request (R"({"id":1,"op":"preview_mix","edit":null})");
        CHECK (reply["ok"] == true);
    }
    SECTION ("bad nested requests")
    {
        struct BadNested
        {
            std::string edit;
            json fields;
        };
        const std::vector<BadNested> cases {
            { R"({"op":"nope"})", { { "key", "edit.op" }, { "expected", "one of its op names" } } },
            { R"({"op":"set_track_volume","track_id":"t"})", { { "key", "edit.volume_db" }, { "expected", "number" } } },
            { R"({"op":"set_master_volume","volume_db":"loud"})", { { "key", "edit.volume_db" }, { "expected", "number" } } },
            { R"({"op":"set_master_volume","volume_db":1.0,"pan":0})", { { "key", "edit.pan" }, { "expected", "no such key" } } },
            { R"({"volume_db":1.0})", { { "key", "edit.op" }, { "expected", "string" } } },
            { R"(5)", { { "key", "edit" }, { "expected", "operation" } } },
            { R"({"op":"add_track"})", { { "key", "edit.op" }, { "expected", "one of its op names" } } },
        };
        for (const auto& bad : cases)
        {
            INFO (bad.edit);
            const auto reply = socket.request (R"({"id":1,"op":"preview_mix","edit":)" + bad.edit + "}");
            CHECK (reply["ok"] == false);
            CHECK (reply["error"]["code"] == "control.bad_field");
            CHECK (reply["error"]["fields"] == bad.fields);
        }
        const auto events = capture.events();
        CHECK (std::ranges::none_of (events, [] (const json& event) { return event.at ("event") == "ui.intent_run"; }));
    }
    CHECK (socket.f.song() == before);
}

TEST_CASE ("set_input_channels takes only a choice the device's inputs offer, and answers others with a quiet ui.input_channels_unavailable", "[control][intent_ops]")
{
    core::ScopedLogCapture capture;
    SocketToWindow socket;
    const auto guitarId = socket.f.trackNamed ("Guitar").id;
    const auto request = [&] (const std::string& channels) {
        return socket.request (R"({"id":1,"op":"set_input_channels","track_id":")" + guitarId + R"(","channels":)" + channels + "}");
    };

    for (const auto* offered : { "[]", "[3]", "[2,3]" })
    {
        INFO (offered);
        CHECK (request (offered)["ok"] == true);
        CHECK (socket.f.trackNamed ("Guitar").settings.inputChannels == json::parse (offered).get<std::vector<int>>());
    }

    const auto before = socket.f.song();
    for (const auto* notOffered : { "[4]", "[-1]", "[1,2]", "[0,0]", "[0,1,2]", "[3,2]" })
    {
        INFO (notOffered);
        const auto reply = request (notOffered);
        REQUIRE (reply["ok"] == false);
        CHECK (reply["error"]["code"] == "ui.input_channels_unavailable");
        CHECK (reply["error"]["fields"]["channels"] == json::parse (notOffered));
        CHECK (reply["error"]["fields"]["input_count"] == 4);
        CHECK (reply["error"]["fields"]["refused"] == false);
    }
    CHECK (socket.f.song() == before);
    const auto events = capture.events();
    CHECK (std::ranges::none_of (events, [] (const json& event) { return event.at ("event") == "ui.notice_shown" || event.at ("event") == "ui.alert_shown"; }));
    CHECK (std::ranges::count_if (events, [] (const json& event) { return event.at ("event") == "ui.input_channels_unavailable" && event.at ("lvl") == "debug"; }) == 6);
}

TEST_CASE ("an absolute path with a control character is a bad field", "[control][intent_ops]")
{
    SocketToWindow socket;

    for (const auto* path : { R"("/tmp/a\u0000b.overdub")", R"("/tmp/a\nb.overdub")", R"("/tmp/a\u001fb.overdub")", R"("/tmp/a\u007fb.overdub")" })
    {
        INFO (path);
        const auto reply = socket.request (std::string (R"({"id":1,"op":"open_song_at","path":)") + path + "}");
        CHECK (reply["ok"] == false);
        CHECK (reply["error"]["code"] == "control.bad_field");
        CHECK (reply["error"]["fields"] == json { { "key", "path" }, { "expected", "absolute path" } });
    }
}

TEST_CASE ("a camera arm waiting for the camera access prompt replies awaiting_answer true", "[control][intent_ops][camera]")
{
    SocketToWindow socket;
    std::function<void (bool)> pendingCameraAccess;
    socket.f.window->askCameraAccess = [&] (std::function<void (bool)> done) { pendingCameraAccess = std::move (done); };

    const auto reply = socket.request (R"({"id":1,"op":"set_camera_armed","armed":true})");

    CHECK (reply == json { { "id", 1 }, { "ok", true }, { "seq", reply["seq"] }, { "created_id", nullptr }, { "awaiting_answer", true } });
    CHECK (pendingCameraAccess != nullptr);
}

TEST_CASE ("set_transpose takes a whole number from -12 to +12 as one undo step, and any other value is an explicit Error", "[control][intent_ops][transpose]")
{
    SocketToWindow socket;

    const auto done = socket.request (R"({"id": 1, "op": "set_transpose", "semitones": -1})");
    CHECK (done.at ("ok") == true);
    CHECK (socket.f.song().transpose == song::Transpose { .semitones = -1 });
    CHECK (socket.f.openSong->package().undoLabel() == "Transpose \xe2\x88\x92"
                                                       "1");

    const auto outOfRange = socket.request (R"({"id": 2, "op": "set_transpose", "semitones": 13})");
    CHECK (outOfRange.at ("ok") == false);
    CHECK (outOfRange.at ("error").at ("code") == "song.transpose_out_of_range");
    CHECK (outOfRange.at ("error").at ("fields").at ("semitones") == 13);

    for (const auto* notWhole : { R"({"id": 3, "op": "set_transpose", "semitones": 1.5})", R"({"id": 4, "op": "set_transpose", "semitones": "+2"})", R"({"id": 5, "op": "set_transpose"})" })
    {
        const auto refused = socket.request (notWhole);
        CHECK (refused.at ("ok") == false);
        CHECK (refused.at ("error").at ("code") == "control.bad_field");
        CHECK (refused.at ("error").at ("fields").at ("key") == "semitones");
    }
    CHECK (socket.f.song().transpose == song::Transpose { .semitones = -1 });
    CHECK (socket.f.openSong->package().undoLabel() == "Transpose \xe2\x88\x92"
                                                       "1");

    // The same value again is no new step.
    CHECK (socket.request (R"({"id": 6, "op": "set_transpose", "semitones": -1})").at ("ok") == true);
    REQUIRE (socket.f.openSong->undo().has_value());
    CHECK (socket.f.song().transpose == song::Transpose {});
    CHECK (socket.f.openSong->package().undoLabel() == "Add Tracks");
}

TEST_CASE ("set_transpose takes a fine tune of whole cents from -50 to +50, 0 when omitted, and any other value is an explicit Error", "[control][intent_ops][transpose]")
{
    SocketToWindow socket;

    CHECK (socket.request (R"({"id": 1, "op": "set_transpose", "semitones": -12, "cents": -50})").at ("ok") == true);
    CHECK (socket.f.song().transpose == song::Transpose { .semitones = -12, .cents = -50 });
    CHECK (socket.f.openSong->package().undoLabel() == "Transpose \xe2\x88\x92"
                                                       "12 \xe2\x88\x92"
                                                       "50 ct");

    CHECK (socket.request (R"({"id": 2, "op": "set_transpose", "semitones": 0, "cents": 30})").at ("ok") == true);
    CHECK (socket.f.song().transpose == song::Transpose { .semitones = 0, .cents = 30 });

    for (const auto* outOfRange : { R"({"id": 3, "op": "set_transpose", "semitones": 0, "cents": 51})", R"({"id": 4, "op": "set_transpose", "semitones": 0, "cents": -51})" })
    {
        const auto refused = socket.request (outOfRange);
        CHECK (refused.at ("ok") == false);
        CHECK (refused.at ("error").at ("code") == "song.transpose_cents_out_of_range");
        CHECK (refused.at ("error").at ("fields").at ("min") == -50);
        CHECK (refused.at ("error").at ("fields").at ("max") == 50);
    }
    const auto notWhole = socket.request (R"({"id": 5, "op": "set_transpose", "semitones": 0, "cents": 2.5})");
    CHECK (notWhole.at ("ok") == false);
    CHECK (notWhole.at ("error").at ("code") == "control.bad_field");
    CHECK (notWhole.at ("error").at ("fields").at ("key") == "cents");
    CHECK (socket.f.song().transpose == song::Transpose { .semitones = 0, .cents = 30 });

    // Omitted cents are 0: the fine tune goes back with the same semitones.
    CHECK (socket.request (R"({"id": 6, "op": "set_transpose", "semitones": 0})").at ("ok") == true);
    CHECK (socket.f.song().transpose == song::Transpose {});

    // Undo restores the previous value.
    REQUIRE (socket.f.openSong->undo().has_value());
    CHECK (socket.f.song().transpose == song::Transpose { .semitones = 0, .cents = 30 });
    REQUIRE (socket.f.openSong->undo().has_value());
    CHECK (socket.f.song().transpose == song::Transpose { .semitones = -12, .cents = -50 });
}

TEST_CASE ("record and export over the socket while Transposing are refused with ui.transposing", "[control][intent_ops][transpose]")
{
    SocketToWindow socket;
    REQUIRE (socket.request (R"({"id": 1, "op": "set_transpose", "semitones": 2})").at ("ok") == true);
    REQUIRE (socket.f.openSong->isTransposing());

    for (const std::string op : { "record", "export" })
    {
        const auto refused = socket.request (R"({"id": 2, "op": ")" + op + R"("})");
        CHECK (refused.at ("ok") == false);
        CHECK (refused.at ("error").at ("code") == "ui.transposing");
        CHECK (refused.at ("error").at ("fields").at ("op") == op);
    }
    CHECK_FALSE (socket.f.openSong->isRecording());
    CHECK_FALSE (socket.f.openSong->isExporting());
}

TEST_CASE ("set_practice_speed takes a whole percent from 25 to 125, outside history, and any other value is an explicit Error", "[control][intent_ops][practice_speed]")
{
    SocketToWindow socket;
    const auto undoLabel = socket.f.openSong->package().undoLabel();

    const auto done = socket.request (R"({"id": 1, "op": "set_practice_speed", "speed_percent": 80})");
    CHECK (done.at ("ok") == true);
    CHECK (socket.f.openSong->listening().practiceSpeed() == 80);
    CHECK (socket.f.openSong->package().undoLabel() == undoLabel);

    const auto slowest = socket.request (R"({"id": 7, "op": "set_practice_speed", "speed_percent": 25})");
    CHECK (slowest.at ("ok") == true);
    CHECK (socket.f.openSong->listening().practiceSpeed() == 25);
    CHECK (socket.f.openSong->package().undoLabel() == undoLabel);
    CHECK (socket.request (R"({"id": 8, "op": "set_practice_speed", "speed_percent": 80})").at ("ok") == true);

    for (const auto* outside : { R"({"id": 2, "op": "set_practice_speed", "speed_percent": 24})", R"({"id": 3, "op": "set_practice_speed", "speed_percent": 126})" })
    {
        const auto outOfRange = socket.request (outside);
        CHECK (outOfRange.at ("ok") == false);
        CHECK (outOfRange.at ("error").at ("code") == "song.practice_speed_out_of_range");
        CHECK (outOfRange.at ("error").at ("fields").at ("min") == 25);
        CHECK (outOfRange.at ("error").at ("fields").at ("max") == 125);
    }

    for (const auto* notWhole : { R"({"id": 4, "op": "set_practice_speed", "speed_percent": 80.5})", R"({"id": 5, "op": "set_practice_speed", "speed_percent": "80%"})", R"({"id": 6, "op": "set_practice_speed"})" })
    {
        const auto refused = socket.request (notWhole);
        CHECK (refused.at ("ok") == false);
        CHECK (refused.at ("error").at ("code") == "control.bad_field");
        CHECK (refused.at ("error").at ("fields").at ("key") == "speed_percent");
    }
    CHECK (socket.f.openSong->listening().practiceSpeed() == 80);
}

TEST_CASE ("set_focus_track takes a Track id, or null or no track_id for none, and set_transcribe_mute and set_transcribe_solo a Track id, all outside history", "[control][intent_ops][transcribe]")
{
    SocketToWindow socket;
    const auto guitarId = socket.f.trackNamed ("Guitar").id;
    const auto bandId = socket.f.trackNamed ("Band Mix").id;
    const auto undoLabel = socket.f.openSong->package().undoLabel();

    CHECK (socket.request (R"({"id": 1, "op": "set_focus_track", "track_id": ")" + guitarId + R"("})").at ("ok") == true);
    CHECK (socket.f.song().settings.focusTrackId == guitarId);
    CHECK (socket.f.trackNamed ("Guitar").settings.transcribeSolo);
    CHECK (socket.request (R"({"id": 2, "op": "set_focus_track", "track_id": null})").at ("ok") == true);
    CHECK_FALSE (socket.f.song().settings.focusTrackId.has_value());
    CHECK (socket.request (R"({"id": 3, "op": "set_focus_track", "track_id": ")" + guitarId + R"("})").at ("ok") == true);
    CHECK (socket.request (R"({"id": 4, "op": "set_focus_track"})").at ("ok") == true);
    CHECK_FALSE (socket.f.song().settings.focusTrackId.has_value());

    CHECK (socket.request (R"({"id": 5, "op": "set_transcribe_mute", "track_id": ")" + bandId + R"(", "mute": true})").at ("ok") == true);
    CHECK (socket.request (R"({"id": 6, "op": "set_transcribe_solo", "track_id": ")" + bandId + R"(", "solo": true})").at ("ok") == true);
    CHECK (socket.f.trackNamed ("Band Mix").settings.transcribeMute);
    CHECK (socket.f.trackNamed ("Band Mix").settings.transcribeSolo);
    CHECK_FALSE (socket.f.trackNamed ("Band Mix").mute);
    CHECK_FALSE (socket.f.trackNamed ("Band Mix").solo);
    CHECK (socket.f.openSong->package().undoLabel() == undoLabel);

    const auto unknown = socket.request (R"({"id": 7, "op": "set_focus_track", "track_id": "track-gone"})");
    CHECK (unknown.at ("ok") == false);
    CHECK (unknown.at ("error").at ("code") == "song.track_not_found");
}

TEST_CASE ("set_octave_shift takes a Track id and the octaves, -1 to +2, outside history", "[control][intent_ops][transcribe][octave]")
{
    SocketToWindow socket;
    const auto bandId = socket.f.trackNamed ("Band Mix").id;
    const auto undoLabel = socket.f.openSong->package().undoLabel();

    CHECK (socket.request (R"({"id": 1, "op": "set_octave_shift", "track_id": ")" + bandId + R"(", "octaves": 2})").at ("ok") == true);
    CHECK (socket.f.trackNamed ("Band Mix").settings.octaveShift == 2);
    CHECK (socket.request (R"({"id": 2, "op": "set_octave_shift", "track_id": ")" + bandId + R"(", "octaves": -1})").at ("ok") == true);
    CHECK (socket.f.trackNamed ("Band Mix").settings.octaveShift == -1);
    CHECK (socket.f.openSong->package().undoLabel() == undoLabel);

    const auto tooHigh = socket.request (R"({"id": 3, "op": "set_octave_shift", "track_id": ")" + bandId + R"(", "octaves": 3})");
    CHECK (tooHigh.at ("ok") == false);
    CHECK (tooHigh.at ("error").at ("code") == "song.octave_shift_out_of_range");
    const auto missing = socket.request (R"({"id": 4, "op": "set_octave_shift", "track_id": ")" + bandId + R"("})");
    CHECK (missing.at ("ok") == false);
    CHECK (missing.at ("error").at ("code") == "control.bad_field");
    CHECK (socket.f.trackNamed ("Band Mix").settings.octaveShift == -1);
}

TEST_CASE ("set_band_filter takes a Track id, on, low_hz and high_hz, outside history, and refuses a low cut not below the high cut", "[control][intent_ops][transcribe][band_filter]")
{
    SocketToWindow socket;
    const auto bandId = socket.f.trackNamed ("Band Mix").id;
    const auto undoLabel = socket.f.openSong->package().undoLabel();

    CHECK (socket.request (R"({"id": 1, "op": "set_band_filter", "track_id": ")" + bandId + R"(", "on": true, "low_hz": 200, "high_hz": 3200})").at ("ok") == true);
    CHECK (socket.f.trackNamed ("Band Mix").settings.bandFilter == overdub::song::BandFilter { .on = true, .lowHz = 200, .highHz = 3200 });
    CHECK (socket.f.openSong->package().undoLabel() == undoLabel);

    const auto inverted = socket.request (R"({"id": 2, "op": "set_band_filter", "track_id": ")" + bandId + R"(", "on": true, "low_hz": 3200, "high_hz": 200})");
    CHECK (inverted.at ("ok") == false);
    CHECK (inverted.at ("error").at ("code") == "song.band_filter_low_not_below_high");
    const auto missing = socket.request (R"({"id": 3, "op": "set_band_filter", "track_id": ")" + bandId + R"(", "on": true, "low_hz": 200})");
    CHECK (missing.at ("ok") == false);
    CHECK (missing.at ("error").at ("code") == "control.bad_field");
    CHECK (socket.f.trackNamed ("Band Mix").settings.bandFilter == overdub::song::BandFilter { .on = true, .lowHz = 200, .highHz = 3200 });
}

TEST_CASE ("add_marker replies with the new Marker's id, select_marker seeks to it, and move_marker, rename_marker and delete_marker each take one undo step", "[control][intent_ops][markers]")
{
    SocketToWindow socket;
    REQUIRE (socket.request (R"({"id":1,"op":"show_mode","mode":"edit"})").at ("ok") == true);
    REQUIRE (socket.request (R"({"id":2,"op":"seek","sample":4800})").at ("ok") == true);

    const auto added = socket.request (R"({"id":3,"op":"add_marker"})");
    REQUIRE (added.at ("ok") == true);
    const auto markerId = added.at ("created_id").get<std::string>();
    REQUIRE (socket.f.song().markers.size() == 1);
    CHECK (socket.f.song().markers[0].id == markerId);
    CHECK (socket.f.song().markers[0].position == 4800);

    CHECK (socket.request (R"({"id":4,"op":"move_marker","marker_id":")" + markerId + R"(","position":9600})").at ("ok") == true);
    CHECK (socket.request (R"({"id":5,"op":"rename_marker","marker_id":")" + markerId + R"(","name":"Solo"})").at ("ok") == true);
    CHECK (socket.f.song().markers[0] == song::Marker { markerId, "Solo", 9600 });
    CHECK (socket.f.openSong->package().undoLabel() == "Rename Marker");

    REQUIRE (socket.request (R"({"id":6,"op":"seek","sample":0})").at ("ok") == true);
    CHECK (socket.request (R"({"id":7,"op":"select_marker","marker_id":")" + markerId + R"("})").at ("ok") == true);
    CHECK (socket.f.openSong->position() == 9600);
    CHECK (socket.f.window->songView()->selectedMarker() == markerId);
    CHECK (socket.request (R"({"id":8,"op":"select_marker"})").at ("ok") == true);
    CHECK_FALSE (socket.f.window->songView()->selectedMarker().has_value());

    CHECK (socket.request (R"({"id":9,"op":"delete_marker","marker_id":")" + markerId + R"("})").at ("ok") == true);
    CHECK (socket.f.song().markers.empty());
    const auto gone = socket.request (R"({"id":10,"op":"select_marker","marker_id":")" + markerId + R"("})");
    CHECK (gone.at ("ok") == false);
    CHECK (gone.at ("error").at ("code") == "song.marker_not_found");

    REQUIRE (socket.request (R"({"id":11,"op":"show_mode","mode":"record"})").at ("ok") == true);
    const auto refused = socket.request (R"({"id":12,"op":"add_marker"})");
    CHECK (refused.at ("ok") == false);
    CHECK (refused.at ("error").at ("code") == "ui.record_mode");
    CHECK (refused.at ("error").at ("message") == "Loops and Markers are made and edited in Transcribe and Edit mode");
}

TEST_CASE ("add_loop replies with the new Loop's id, select_loop activates it in Transcribe mode, set_looping turns it off, and resize_loop, rename_loop and delete_loop each take one undo step", "[control][intent_ops][loops]")
{
    SocketToWindow socket;
    REQUIRE (socket.request (R"({"id":1,"op":"show_mode","mode":"transcribe"})").at ("ok") == true);

    const auto added = socket.request (R"({"id":2,"op":"add_loop","start":9600,"end":4800})");
    REQUIRE (added.at ("ok") == true);
    const auto loopId = added.at ("created_id").get<std::string>();
    REQUIRE (socket.f.song().loops.size() == 1);
    CHECK (socket.f.song().loops[0] == song::Loop { loopId, "Loop 1", 4800, 9600 });
    CHECK (socket.f.song().settings.activeLoopId == loopId);
    CHECK (socket.f.song().settings.looping);

    CHECK (socket.request (R"({"id":3,"op":"set_looping","looping":false})").at ("ok") == true);
    CHECK_FALSE (socket.f.song().settings.looping);
    CHECK (socket.request (R"({"id":4,"op":"select_loop","loop_id":")" + loopId + R"("})").at ("ok") == true);
    CHECK (socket.f.song().settings.looping);
    CHECK (socket.f.openSong->position() == 4800);

    CHECK (socket.request (R"({"id":5,"op":"resize_loop","loop_id":")" + loopId + R"(","start":0,"end":24000})").at ("ok") == true);
    CHECK (socket.request (R"({"id":6,"op":"rename_loop","loop_id":")" + loopId + R"(","name":"Riff"})").at ("ok") == true);
    CHECK (socket.f.song().loops[0] == song::Loop { loopId, "Riff", 0, 24000 });
    CHECK (socket.f.openSong->package().undoLabel() == "Rename Loop");

    CHECK (socket.request (R"({"id":7,"op":"mark_loop_start"})").at ("ok") == true);
    CHECK (socket.request (R"({"id":8,"op":"cancel_loop_start"})").at ("ok") == true);
    CHECK (socket.request (R"({"id":9,"op":"select_adjacent_loop","direction":"back"})").at ("ok") == true);

    CHECK (socket.request (R"({"id":10,"op":"delete_loop","loop_id":")" + loopId + R"("})").at ("ok") == true);
    CHECK (socket.f.song().loops.empty());
    const auto gone = socket.request (R"({"id":11,"op":"select_loop","loop_id":")" + loopId + R"("})");
    CHECK (gone.at ("ok") == false);
    CHECK (gone.at ("error").at ("code") == "song.loop_not_found");
    const auto noLoop = socket.request (R"({"id":12,"op":"set_looping","looping":true})");
    CHECK (noLoop.at ("error").at ("code") == "song.no_active_loop");

    REQUIRE (socket.request (R"({"id":13,"op":"show_mode","mode":"record"})").at ("ok") == true);
    const auto refused = socket.request (R"({"id":14,"op":"add_loop","start":0,"end":100})");
    CHECK (refused.at ("error").at ("code") == "ui.record_mode");
}

TEST_CASE ("set_export_length takes a loop_id outside history, and refuses a Loop the Song does not have", "[control][intent_ops][loops][export]")
{
    SocketToWindow socket;
    REQUIRE (socket.request (R"({"id":1,"op":"show_mode","mode":"transcribe"})").at ("ok") == true);
    const auto loopId = socket.request (R"({"id":2,"op":"add_loop","start":4800,"end":9600})").at ("created_id").get<std::string>();
    const auto undoLabel = socket.f.openSong->package().undoLabel();

    CHECK (socket.request (R"({"id":3,"op":"set_export_length","length":"full_song","loop_id":")" + loopId + R"("})").at ("ok") == true);
    CHECK (socket.f.song().settings.exportLoopId == loopId);
    CHECK (socket.f.openSong->package().undoLabel() == undoLabel);

    const auto gone = socket.request (R"({"id":5,"op":"set_export_length","length":"full_song","loop_id":"loop-gone"})");
    CHECK (gone.at ("ok") == false);
    CHECK (gone.at ("error").at ("code") == "song.loop_not_found");
    CHECK (socket.f.song().settings.exportLoopId == loopId);

    CHECK (socket.request (R"({"id":6,"op":"set_export_length","length":"full_song"})").at ("ok") == true);
    CHECK_FALSE (socket.f.song().settings.exportLoopId.has_value());
}

TEST_CASE ("the socket shows Transcribe mode, and a Clip edit there answers ui.transcribing with its reason", "[control][intent_ops][transcribe]")
{
    SocketToWindow socket;
    const auto clipId = socket.f.trackNamed ("Band Mix").clips.front().id;
    REQUIRE (socket.request (R"({"id":1,"op":"select_clip","clip_id":")" + clipId + R"("})").at ("ok") == true);
    REQUIRE (socket.request (R"({"id":2,"op":"seek","sample":4800})").at ("ok") == true);

    const core::ScopedLogCapture capture;
    CHECK (socket.request (R"({"id":3,"op":"show_mode","mode":"transcribe"})").at ("ok") == true);
    CHECK (socket.f.window->mode() == ui::Mode::transcribe);
    const auto events = capture.events();
    const auto changed = std::ranges::find_if (events, [] (const json& event) { return event.at ("event") == "ui.mode_changed"; });
    REQUIRE (changed != events.end());
    CHECK (changed->at ("mode") == "transcribe");

    const auto before = socket.f.song();
    const auto refused = socket.request (R"({"id":4,"op":"split_selected_clip"})");
    CHECK (refused.at ("ok") == false);
    CHECK (refused.at ("error").at ("code") == "ui.transcribing");
    CHECK (refused.at ("error").at ("fields").at ("op") == "split_selected_clip");
    CHECK (refused.at ("error").at ("message") == "Transcribe mode never changes a Clip");
    CHECK (socket.f.song() == before);

    CHECK (socket.request (R"({"id":5,"op":"set_transcribe_column_visible","visible":false})").at ("ok") == true);
    CHECK_FALSE (socket.f.window->isTranscribeColumnVisible());
    CHECK (socket.request (R"({"id":6,"op":"show_mode","mode":"listen"})").at ("error").at ("code") == "control.bad_field");
}
