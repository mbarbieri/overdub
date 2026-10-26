#include "ui/Availability.h"

#include "core/Described.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <map>
#include <set>
#include <string>

using namespace overdub;
using namespace overdub::ui;

namespace
{
// Every op's refusal code now, none for the ops that may run.
std::map<std::string, std::string> refusedCodes (const Running& running)
{
    std::map<std::string, std::string> codes;
    for (const auto op : core::operationNames<Intent>())
        if (const auto refused = refusalOf (op, running))
            codes[std::string (op)] = refused->error.code;
    return codes;
}

std::set<std::string> allOps()
{
    std::set<std::string> ops;
    for (const auto op : core::operationNames<Intent>())
        ops.insert (std::string (op));
    return ops;
}

// Every op but these, each refused with `code`.
std::map<std::string, std::string> allBut (const std::set<std::string>& allowed, const std::string& code)
{
    std::map<std::string, std::string> codes;
    for (const auto& op : allOps())
        if (! allowed.contains (op))
            codes[op] = code;
    return codes;
}

const std::set<std::string> appOps { "new_song_at", "open_song_at", "duplicate_song_to", "convert_sample_rate", "show_settings", "apply_device_settings", "measure_latency", "save_latency_calibration", "measure_clap_pass", "save_camera_calibration", "quit" };
} // namespace

TEST_CASE ("nothing is refused while nothing runs", "[ui][availability]")
{
    CHECK (refusedCodes (Running {}).empty());
    CHECK (refusedCodes (Running { .song = ActivityKind::idle }).empty());
}

TEST_CASE ("with no Song open a latency calibration refuses only the App group but quit", "[ui][availability][calibration]")
{
    std::map<std::string, std::string> expected;
    for (const auto& op : appOps)
        if (op != "quit")
            expected[op] = "ui.calibration_running";

    CHECK (refusedCodes (Running { .calibrating = true }) == expected);
}

TEST_CASE ("during a Pass only transport, view and plugin window ops, the Window group and quit may run", "[ui][availability][record]")
{
    const std::set<std::string> allowed {
        "record", "play_or_stop", "rewind", "cancel_run", "open_insert_editor", "close_plugin_window",
        "set_export_format", "set_export_length", "set_export_destination", "set_export_video_take", "reveal_export",
        "select_clip", "select_video_take", "set_timeline_view", "set_vertical_scroll", "set_take_lanes_unfolded",
        "set_mixer_visible", "set_mixer_height", "set_passes_panel_open", "clear_clip_indicator",
        "show_mode", "dismiss_alerts", "quit"
    };

    CHECK (refusedCodes (Running { .song = ActivityKind::pass }) == allBut (allowed, "ui.stop_recording_first"));
}

TEST_CASE ("during an export only record, play, import, Track delete, Separation and conversion are refused", "[ui][availability][export]")
{
    const std::map<std::string, std::string> refused {
        { "record", "ui.export_running" },
        { "play_or_stop", "ui.export_running" },
        { "import_files", "ui.export_running" },
        { "delete_track", "ui.export_running" },
        { "start_separation", "ui.export_running" },
        { "convert_sample_rate", "ui.export_running" },
    };

    CHECK (refusedCodes (Running { .song = ActivityKind::exportSong }) == refused);
}

TEST_CASE ("under the run dialog only cancel_run, close_plugin_window and the App group pass, and the App group waits for the run", "[ui][availability][separation]")
{
    auto allowed = appOps;
    allowed.insert ("cancel_run");
    allowed.insert ("close_plugin_window");

    for (const auto& [kind, appCode] : { std::pair { ActivityKind::separation, "ui.separation_running" }, std::pair { ActivityKind::conversion, "ui.conversion_running" } })
    {
        auto expected = allBut (allowed, "ui.run_dialog_open");
        for (const auto& op : appOps)
            expected[op] = appCode;
        if (kind == ActivityKind::conversion)
            expected.erase ("quit"); // only the Separation dialog holds quit

        CHECK (refusedCodes (Running { .song = kind }) == expected);
    }
}

TEST_CASE ("a latency calibration refuses the App group but quit, and the Song ops that need the audio device", "[ui][availability][calibration]")
{
    std::map<std::string, std::string> expected;
    for (const auto& op : appOps)
        if (op != "quit")
            expected[op] = "ui.calibration_running";
    for (const auto* op : { "record", "play_or_stop", "set_monitoring", "export" })
        expected[op] = "ui.calibration_running";

    CHECK (refusedCodes (Running { .song = ActivityKind::idle, .calibrating = true }) == expected);
}

TEST_CASE ("a latency calibration beside an export refuses with the calibration first, then the export", "[ui][availability][calibration][export]")
{
    const Running running { .song = ActivityKind::exportSong, .calibrating = true };

    CHECK (refusalOf ("record", running)->error.code == "ui.calibration_running");
    CHECK (refusalOf ("import_files", running)->error.code == "ui.export_running");
    CHECK_FALSE (refusalOf ("undo", running).has_value());
}

TEST_CASE ("a refusal is presented for the App group, record, export and start_separation, and logged quietly otherwise", "[ui][availability]")
{
    const auto presented = [] (std::string_view op, const Running& running) { return refusalOf (op, running)->presented; };
    const Running pass { .song = ActivityKind::pass };

    CHECK (presented ("new_song_at", pass));
    CHECK (presented ("start_separation", pass));
    CHECK (presented ("export", pass));
    CHECK (presented ("record", Running { .song = ActivityKind::exportSong }));
    CHECK (presented ("record", Running { .song = ActivityKind::idle, .calibrating = true }));
    CHECK_FALSE (presented ("undo", pass));
    CHECK_FALSE (presented ("set_monitoring", Running { .song = ActivityKind::idle, .calibrating = true }));
    CHECK_FALSE (presented ("undo", Running { .song = ActivityKind::separation }));
}

TEST_CASE ("a refusal names the op of an App Intent and the gesture of a Song Intent", "[ui][availability]")
{
    const Running pass { .song = ActivityKind::pass };

    CHECK (refusalOf ("open_song_at", pass)->error.fields == core::Fields { { "op", "open_song_at" } });
    CHECK (refusalOf ("move_clip", pass)->error.fields == core::Fields { { "gesture", "edit" } });
    CHECK (refusalOf ("export", Running { .song = ActivityKind::idle, .calibrating = true })->error.fields == core::Fields { { "gesture", "start_export" } });
    CHECK (refusalOf ("undo", Running { .song = ActivityKind::conversion })->error.fields == core::Fields { { "op", "undo" } });
}

TEST_CASE ("hiding the Settings window is never refused, showing it waits", "[ui][availability][calibration]")
{
    const Running calibrating { .song = ActivityKind::idle, .calibrating = true };

    CHECK_FALSE (refusalOf (Intent { AppIntent { intent::ShowSettings { false } } }, calibrating).has_value());
    CHECK (refusalOf (Intent { AppIntent { intent::ShowSettings { true } } }, calibrating)->error.code == "ui.calibration_running");
}
