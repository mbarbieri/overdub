#pragma once

#include "song/Song.h"

#include <string>

// The top bar's texts (issue #27): the transport clock in seconds, its status line (REC · PASS n
// while a Pass records) and the Undo / Redo button captions naming the step. Pure.
namespace overdub::ui
{
struct TransportStatus
{
    bool playing = false;
    bool recording = false; // a Pass is being recorded (or is being finished)
    int passNumber = 0;     // the recording Pass's number, from 1
};

// "83.4 s": the Song position in seconds, rounded down to a tenth. Negative positions and an
// unknown rate (<= 0) read "0.0 s".
std::string clockText (song::SampleCount position, int sampleRate);

// "1.250 s": samples as seconds with three decimals; an unknown rate (<= 0) reads "0.000 s".
std::string secondsText (song::SampleCount samples, int sampleRate);

// "REC · PASS n" while recording, else "PLAY" or "STOP".
std::string statusText (TransportStatus status);

// "Undo <label>" / "Redo <label>", or plain "Undo" / "Redo" when there is no step.
std::string undoButtonText (const std::string& label);
std::string redoButtonText (const std::string& label);
} // namespace overdub::ui
