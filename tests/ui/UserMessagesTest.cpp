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

    CHECK (tierFor ("capture.take_write_failed") == Tier::alert);
    CHECK (tierFor ("song.import_unreadable") == Tier::alert);
    CHECK (tierFor ("song.write_failed") == Tier::alert);
    CHECK (tierFor ("song.some_future_code") == Tier::alert);
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
    CHECK (queue.hasDeferredAlerts());

    CHECK (queue.takeReady (true).empty());

    const auto afterPass = queue.takeReady (false);
    REQUIRE (afterPass.size() == 1);
    CHECK (afterPass[0].code == "capture.take_write_failed");
    CHECK (afterPass[0].tier == Tier::alert);
    CHECK (! queue.hasDeferredAlerts());
    CHECK (queue.takeReady (false).empty());
}
