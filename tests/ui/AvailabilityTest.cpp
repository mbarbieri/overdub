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

const std::set<std::string> appOps { "new_song_at", "open_song_at", "reopen_last_song", "duplicate_song_to", "convert_sample_rate", "show_settings", "apply_device_settings", "measure_latency", "save_latency_calibration", "measure_clap_pass", "save_camera_calibration", "quit" };
} // namespace

TEST_CASE ("while nothing runs only cancel_export is refused, and with no Song open nothing is", "[ui][availability]")
{
    CHECK (refusedCodes (Running {}).empty());
    CHECK (refusedCodes (Running { .song = ActivityKind::idle }) == std::map<std::string, std::string> { { "cancel_export", "ui.no_export_running" } });
}

TEST_CASE ("cancel_export may run only while an export runs, beside a calibration too", "[ui][availability][export]")
{
    CHECK_FALSE (refusalOf ("cancel_export", Running { .song = ActivityKind::exportSong }).has_value());
    CHECK_FALSE (refusalOf ("cancel_export", Running { .song = ActivityKind::exportSong, .calibrating = true }).has_value());
    for (const auto kind : { ActivityKind::idle, ActivityKind::pass })
    {
        const auto refused = refusalOf ("cancel_export", Running { .song = kind });
        REQUIRE (refused.has_value());
        CHECK (refused->error.code == "ui.no_export_running");
        CHECK (refused->error.fields == core::Fields { { "op", "cancel_export" } });
        CHECK_FALSE (refused->presented);
    }
}

TEST_CASE ("record_again is refused, quietly, while the Song has no Pass and none runs", "[ui][availability][record_again]")
{
    const auto refused = refusalOf ("record_again", Running { .song = ActivityKind::idle, .songHasNoPass = true });
    REQUIRE (refused.has_value());
    CHECK (refused->error.code == "ui.no_pass_to_record_again");
    CHECK (refused->error.fields == core::Fields { { "op", "record_again" } });
    CHECK_FALSE (refused->presented);
    CHECK (refusedCodes (Running { .song = ActivityKind::idle, .songHasNoPass = true }) == std::map<std::string, std::string> { { "cancel_export", "ui.no_export_running" }, { "record_again", "ui.no_pass_to_record_again" } });
    CHECK_FALSE (refusalOf ("record_again", Running { .song = ActivityKind::pass, .songHasNoPass = true }).has_value());
    CHECK_FALSE (refusalOf ("record_again", Running { .song = ActivityKind::idle }).has_value());
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
        "record", "record_again", "play_or_stop", "rewind", "cancel_run", "open_insert_editor", "close_plugin_window",
        "set_export_format", "set_export_length", "set_export_destination", "set_export_video_take", "reveal_export",
        "select_clip", "select_video_take", "set_timeline_view", "set_transcribe_view", "set_vertical_scroll", "set_take_lanes_unfolded",
        "set_mixer_visible", "set_mixer_height", "set_passes_panel_open", "clear_clip_indicator", "cancel_loop_start",
        "show_mode", "dismiss_alerts", "set_transcribe_column_visible", "quit"
    };

    auto expected = allBut (allowed, "ui.stop_recording_first");
    expected["cancel_export"] = "ui.no_export_running";
    CHECK (refusedCodes (Running { .song = ActivityKind::pass }) == expected);
}

TEST_CASE ("during an export only record, record again, play, import, Track delete, Separation, conversion and opening the Export sheet are refused", "[ui][availability][export]")
{
    const std::map<std::string, std::string> refused {
        { "record", "ui.export_running" },
        { "record_again", "ui.export_running" },
        { "play_or_stop", "ui.export_running" },
        { "import_files", "ui.export_running" },
        { "delete_track", "ui.export_running" },
        { "start_separation", "ui.export_running" },
        { "convert_sample_rate", "ui.export_running" },
        { "show_export", "ui.export_running" },
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
    for (const auto* op : { "record", "record_again", "play_or_stop", "set_monitoring", "export", "show_export" })
        expected[op] = "ui.calibration_running";
    expected["cancel_export"] = "ui.no_export_running";

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

TEST_CASE ("closing the Export sheet is never refused, opening it waits until nothing runs", "[ui][availability][export]")
{
    for (const auto kind : { ActivityKind::pass, ActivityKind::exportSong, ActivityKind::separation })
    {
        const Running running { .song = kind };
        CHECK_FALSE (refusalOf (Intent { SongIntent { intent::ShowExport { false } } }, running).has_value());
        const auto opening = refusalOf (Intent { SongIntent { intent::ShowExport { true } } }, running);
        REQUIRE (opening.has_value());
        CHECK_FALSE (opening->presented);
    }
    CHECK_FALSE (refusalOf (Intent { SongIntent { intent::ShowExport { true } } }, Running { .song = ActivityKind::idle }).has_value());
}

TEST_CASE ("hiding the Settings window is never refused, showing it waits", "[ui][availability][calibration]")
{
    const Running calibrating { .song = ActivityKind::idle, .calibrating = true };

    CHECK_FALSE (refusalOf (Intent { AppIntent { intent::ShowSettings { false } } }, calibrating).has_value());
    CHECK (refusalOf (Intent { AppIntent { intent::ShowSettings { true } } }, calibrating)->error.code == "ui.calibration_running");
}

TEST_CASE ("while Transposing only record, record_again, show_export and export wait for it, the gestures presented as its Notice", "[ui][availability][transpose]")
{
    const Running transposing { .song = ActivityKind::idle, .transposing = true };
    auto codes = refusedCodes (transposing);
    codes.erase ("cancel_export");
    CHECK (codes == std::map<std::string, std::string> { { "record", "ui.transposing" }, { "record_again", "ui.transposing" }, { "show_export", "ui.transposing" }, { "export", "ui.transposing" } });
    const auto refused = refusalOf ("record", transposing);
    REQUIRE (refused.has_value());
    CHECK (refused->presented);
    CHECK (refused->error.fields.at ("op") == "record");
    CHECK (refusalOf ("export", transposing)->presented);
    CHECK_FALSE (refusalOf ("show_export", transposing)->presented);
    CHECK (unavailableReason ("ui.transposing") == "Transposing\xe2\x80\xa6");
    CHECK (refusalOf ("set_transpose", Running { .song = ActivityKind::pass }).has_value());
    CHECK_FALSE (refusalOf ("set_transpose", Running { .song = ActivityKind::idle, .transposing = true }).has_value());
}

TEST_CASE ("set_practice_speed is refused while a Pass, a Separation or a conversion runs, and allowed during an export", "[ui][availability][practice_speed]")
{
    CHECK (refusalOf ("set_practice_speed", Running { .song = ActivityKind::pass })->error.code == "ui.stop_recording_first");
    CHECK (refusalOf ("set_practice_speed", Running { .song = ActivityKind::separation }).has_value());
    CHECK (refusalOf ("set_practice_speed", Running { .song = ActivityKind::conversion }).has_value());
    CHECK_FALSE (refusalOf ("set_practice_speed", Running { .song = ActivityKind::exportSong }).has_value());
    CHECK_FALSE (refusalOf ("set_practice_speed", Running { .song = ActivityKind::idle, .transposing = true }).has_value());
}

TEST_CASE ("the Focus Track and Transcribe Mix ops are refused while a Pass, a Separation or a conversion runs, and allowed during an export and in every mode", "[ui][availability][transcribe]")
{
    for (const auto* op : { "set_focus_track", "set_transcribe_mute", "set_transcribe_solo", "set_octave_shift", "set_band_filter" })
    {
        INFO (op);
        CHECK (refusalOf (op, Running { .song = ActivityKind::pass })->error.code == "ui.stop_recording_first");
        CHECK (refusalOf (op, Running { .song = ActivityKind::separation }).has_value());
        CHECK (refusalOf (op, Running { .song = ActivityKind::conversion }).has_value());
        CHECK_FALSE (refusalOf (op, Running { .song = ActivityKind::exportSong }).has_value());
        CHECK_FALSE (refusalOf (op, Running { .song = ActivityKind::idle }).has_value());
        CHECK_FALSE (refusalOf (op, Running { .song = ActivityKind::idle, .transcribing = true }).has_value());
    }
}

TEST_CASE ("in Transcribe mode the Clip edits and nudge are refused quietly with ui.transcribing", "[ui][availability][transcribe]")
{
    const std::set<std::string> clipOps { "move_clip", "trim_clip_start", "trim_clip_end", "set_clip_fade_in", "set_clip_fade_out", "set_clip_gain", "split_selected_clip", "delete_selection", "take_lane_click", "nudge" };
    std::map<std::string, std::string> expected { { "cancel_export", "ui.no_export_running" } };
    for (const auto& op : clipOps)
        expected[op] = "ui.transcribing";
    CHECK (refusedCodes (Running { .song = ActivityKind::idle, .transcribing = true }) == expected);

    const auto refused = refusalOf ("split_selected_clip", Running { .song = ActivityKind::idle, .transcribing = true });
    REQUIRE (refused.has_value());
    CHECK_FALSE (refused->presented);
    CHECK (refused->error.fields.at ("op") == "split_selected_clip");
    CHECK (unavailableReason ("ui.transcribing") == "Unavailable in Transcribe mode");
    // With no Song open each answers ui.no_song_open from its handler instead.
    CHECK (refusedCodes (Running { .transcribing = true }).empty());
}

TEST_CASE ("Record mode refuses adding and editing Markers with ui.record_mode, presented; Transcribe and Edit allow them", "[ui][availability][markers]")
{
    for (const auto* op : { "add_marker", "move_marker", "rename_marker", "delete_marker" })
    {
        INFO (op);
        const auto refused = refusalOf (op, Running { .song = ActivityKind::idle, .recordMode = true });
        REQUIRE (refused.has_value());
        CHECK (refused->error.code == "ui.record_mode");
        CHECK (refused->error.fields.at ("op") == op);
        CHECK (refused->presented);
        CHECK_FALSE (refusalOf (op, Running { .song = ActivityKind::idle }).has_value());
        CHECK_FALSE (refusalOf (op, Running { .song = ActivityKind::idle, .transcribing = true }).has_value());
        CHECK (refusalOf (op, Running { .song = ActivityKind::separation }).has_value());
        CHECK_FALSE (refusalOf (op, Running { .recordMode = true }).has_value()); // no Song: ui.no_song_open instead
    }
    CHECK (unavailableReason ("ui.record_mode") == "Unavailable in Record mode");
    // Seeking to a Marker works in every mode.
    CHECK_FALSE (refusalOf ("select_marker", Running { .song = ActivityKind::idle, .recordMode = true }).has_value());
    CHECK (refusalOf ("select_marker", Running { .song = ActivityKind::pass, .recordMode = true }).has_value());
}

TEST_CASE ("Record mode refuses making and editing Loops with ui.record_mode, presented; selecting one works in every mode", "[ui][availability][loops]")
{
    for (const auto* op : { "add_loop", "mark_loop_start", "mark_loop_end", "resize_loop", "rename_loop", "delete_loop" })
    {
        INFO (op);
        const auto refused = refusalOf (op, Running { .song = ActivityKind::idle, .recordMode = true });
        REQUIRE (refused.has_value());
        CHECK (refused->error.code == "ui.record_mode");
        CHECK (refused->presented);
        CHECK_FALSE (refusalOf (op, Running { .song = ActivityKind::idle }).has_value());
        CHECK_FALSE (refusalOf (op, Running { .song = ActivityKind::idle, .transcribing = true }).has_value());
    }
    for (const auto* op : { "select_loop", "select_adjacent_loop", "set_looping", "cancel_loop_start" })
    {
        INFO (op);
        CHECK_FALSE (refusalOf (op, Running { .song = ActivityKind::idle, .recordMode = true }).has_value());
    }
    CHECK (refusalOf ("select_loop", Running { .song = ActivityKind::pass, .recordMode = true }).has_value());
}

TEST_CASE ("an Error is a refusal only with an activity's refusal code and the field gesture", "[ui][availability]")
{
    const auto refused = refusalOf ("record", Running { .song = ActivityKind::exportSong });
    REQUIRE (refused.has_value());
    CHECK (isRefusal (refused->error));
    CHECK (isRefusal (refusalOf ("undo", Running { .song = ActivityKind::pass })->error));
    CHECK (isRefusal (refusalOf ("record", Running { .song = ActivityKind::idle, .calibrating = true })->error));
    CHECK_FALSE (isRefusal (refusalOf ("record", Running { .song = ActivityKind::separation })->error)); // the run dialog's
    CHECK_FALSE (isRefusal (core::Error { "ui.export_running", "an export is running", {} }));
    CHECK_FALSE (isRefusal (core::Error { "song.track_not_found", "no such Track", { { "gesture", "edit" } } }));
}
