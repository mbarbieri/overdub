#pragma once

#include "song/Song.h"

#include <optional>
#include <string>

// The top bar's texts (issue #27): the transport clock in seconds, its status line (REC · PASS n
// while a Pass records, PRE-ROLL · 1.9 s while it waits for its record point, issue #32) and the
// Undo / Redo button captions naming the step. Pure.
namespace overdub::ui
{
struct TransportStatus
{
    bool playing = false;
    bool recording = false; // a Pass is being recorded
    bool saving = false;    // a stopped Pass is being saved (its movie completing); not recording
    int passNumber = 0;     // the recording Pass's number, from 1
    std::optional<song::SampleCount> preRollRemaining; // samples to the record point during pre-roll
    int sampleRate = 0;
};

// "83.4 s": the Song position in seconds, rounded down to a tenth. Negative positions and an
// unknown rate (<= 0) read "0.0 s".
std::string clockText (song::SampleCount position, int sampleRate);

// "1.250 s": samples as seconds with three decimals; an unknown rate (<= 0) reads "0.000 s".
std::string secondsText (song::SampleCount samples, int sampleRate);

// "PRE-ROLL · 1.9 s" while pre-roll counts down to the record point, "REC · PASS n" while
// recording, "SAVING · PASS n" while a stopped Pass is saved (issue #91), else "PLAY" or "STOP".
std::string statusText (TransportStatus status);

// The pre-roll countdown, rounded up to a tenth so it reads 0.0 only at the record point: "1.9 s".
std::string preRollCountdownText (song::SampleCount remaining, int sampleRate);

// "Undo <label>" / "Redo <label>", or plain "Undo" / "Redo" when there is no step.
std::string undoButtonText (const std::string& label);
std::string redoButtonText (const std::string& label);
} // namespace overdub::ui
