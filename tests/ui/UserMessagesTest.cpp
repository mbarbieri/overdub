#include "ui/UserMessages.h"

#include <catch2/catch_test_macros.hpp>

using namespace overdub::ui;

TEST_CASE ("situations the user need not decide on are Notices; lost data and failed actions are Alerts", "[ui][messages]")
{
    CHECK (tierFor ("song.history_dropped") == Tier::notice);
    CHECK (tierFor ("song.backing_track_missing") == Tier::notice);
    CHECK (tierFor ("capture.nothing_armed") == Tier::notice);
    CHECK (tierFor ("capture.pass_empty") == Tier::notice);
    CHECK (tierFor ("ui.no_audio_device") == Tier::notice);
    CHECK (tierFor ("ui.stop_recording_first") == Tier::notice);
    CHECK (tierFor ("song.split_outside_clip") == Tier::notice);
    CHECK (tierFor ("song.clip_not_found") == Tier::notice);
    CHECK (tierFor ("ui.latency_not_calibrated") == Tier::notice);
    CHECK (tierFor ("ui.calibration_running") == Tier::notice);
    CHECK (tierFor ("capture.calibration_timeout") == Tier::notice);
    CHECK (tierFor ("capture.clap_no_video_take") == Tier::notice);

    CHECK (tierFor ("capture.take_write_failed") == Tier::alert);
    CHECK (tierFor ("song.import_unreadable") == Tier::alert);
    CHECK (tierFor ("song.write_failed") == Tier::alert);
    CHECK (tierFor ("capture.stop_timeout") == Tier::alert);
    CHECK (tierFor ("engine.device_settings_write_failed") == Tier::alert);
    CHECK (tierFor ("song.some_future_code") == Tier::alert);
}

TEST_CASE ("a crashed plugin is a Notice; an export it spoiled is an Alert", "[ui][messages][crash]")
{
    CHECK (tierFor ("ui.plugin_crashed") == Tier::notice);
    CHECK (tierFor ("ui.plugin_crashed_repeatedly") == Tier::notice);
    CHECK (tierFor ("ui.export_plugin_crashed") == Tier::alert);
    for (const auto* code : { "ui.plugin_crashed", "ui.plugin_crashed_repeatedly", "ui.export_plugin_crashed" })
        CHECK (sentenceFor (code).find (code) == std::string::npos);
}

TEST_CASE ("every code reads as a sentence; an unknown code falls back to one naming it", "[ui][messages]")
{
    CHECK (sentenceFor ("song.history_dropped") != sentenceFor ("capture.nothing_armed"));
    CHECK (sentenceFor ("song.history_dropped").find ("song.history_dropped") == std::string::npos);
    CHECK (sentenceFor ("song.some_future_code").find ("song.some_future_code") != std::string::npos);
}

TEST_CASE ("Alerts wait while a Pass runs; Notices show at once", "[ui][messages]")
{
    UserMessageQueue queue;
    queue.post (userMessageFor ("capture.take_write_failed"));
    queue.post (userMessageFor ("capture.nothing_armed"));

    const auto duringPass = queue.takeReady (true);
    REQUIRE (duringPass.size() == 1);
    CHECK (duringPass[0].code == "capture.nothing_armed");
    CHECK (duringPass[0].tier == Tier::notice);

    CHECK (queue.takeReady (true).empty());

    const auto afterPass = queue.takeReady (false);
    REQUIRE (afterPass.size() == 1);
    CHECK (afterPass[0].code == "capture.take_write_failed");
    CHECK (afterPass[0].tier == Tier::alert);
    CHECK (queue.takeReady (false).empty());
}

TEST_CASE ("device changes the user need not decide on are Notices; a Pass cut short by the audio device is an Alert", "[ui][messages][device_change]")
{
    CHECK (tierFor ("ui.audio_device_lost") == Tier::notice);
    CHECK (tierFor ("ui.sample_rate_mismatch") == Tier::notice);
    CHECK (tierFor ("ui.camera_disconnected") == Tier::notice);
    CHECK (tierFor ("ui.audio_device_lost_during_pass") == Tier::alert);
    for (const auto* code : { "ui.audio_device_lost", "ui.sample_rate_mismatch", "ui.camera_disconnected", "ui.audio_device_lost_during_pass" })
        CHECK (sentenceFor (code).find (code) == std::string::npos);
}
