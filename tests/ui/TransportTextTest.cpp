#include "ui/TransportText.h"

#include <catch2/catch_test_macros.hpp>

using overdub::ui::clockText;
using overdub::ui::redoButtonText;
using overdub::ui::statusText;
using overdub::ui::TransportStatus;
using overdub::ui::undoButtonText;

TEST_CASE ("the clock shows the playhead in seconds, rounded down to a tenth", "[ui][transport]")
{
    CHECK (clockText (0, 48000) == "0.0 s");
    CHECK (clockText (47999, 48000) == "0.9 s");
    CHECK (clockText (48000 * 83 + 19200, 48000) == "83.4 s");
    CHECK (clockText (44100 * 2, 44100) == "2.0 s");
    CHECK (clockText (-100, 48000) == "0.0 s");
    CHECK (clockText (1000, 0) == "0.0 s");
}

TEST_CASE ("the clock's status line reads REC · PASS n while a Pass records", "[ui][transport]")
{
    CHECK (statusText (TransportStatus { .playing = true, .recording = true, .passNumber = 3 }) == "REC \xC2\xB7 PASS 3");
    CHECK (statusText (TransportStatus { .playing = true }) == "PLAY");
    CHECK (statusText (TransportStatus {}) == "STOP");
}

TEST_CASE ("Undo and Redo buttons name the step they would undo or redo", "[ui][transport]")
{
    CHECK (undoButtonText ("Record Pass 1") == "Undo Record Pass 1");
    CHECK (undoButtonText ("") == "Undo");
    CHECK (redoButtonText ("Import Band Mix.wav") == "Redo Import Band Mix.wav");
    CHECK (redoButtonText ("") == "Redo");
}
