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
    CHECK (*showMode == json { { "op", "show_mode" }, { "group", "window" }, { "fields", json::array ({ { { "key", "mode" }, { "type", "string" }, { "enum", { "record", "edit", "export" } } } }) } });

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
        CHECK (socket.f.trackNamed ("Guitar").inputChannels == json::parse (offered).get<std::vector<int>>());
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
