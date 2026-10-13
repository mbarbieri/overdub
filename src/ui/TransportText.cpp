#include "ui/TransportText.h"

#include <format>

namespace overdub::ui
{
std::string clockText (song::SampleCount position, int sampleRate)
{
    if (position <= 0 || sampleRate <= 0)
        return "0.0 s";
    const auto tenths = position * 10 / sampleRate;
    return std::format ("{}.{} s", tenths / 10, tenths % 10);
}

std::string statusText (TransportStatus status)
{
    if (status.preRollRemaining.has_value())
        return "PRE-ROLL \xC2\xB7 " + preRollCountdownText (*status.preRollRemaining, status.sampleRate);
    if (status.recording)
        return std::format ("REC \xC2\xB7 PASS {}", status.passNumber);
    if (status.saving)
        return std::format ("SAVING \xC2\xB7 PASS {}", status.passNumber);
    return status.playing ? "PLAY" : "STOP";
}

std::string preRollCountdownText (song::SampleCount remaining, int sampleRate)
{
    const auto tenths = remaining <= 0 || sampleRate <= 0 ? 0 : (remaining * 10 + sampleRate - 1) / sampleRate;
    return std::format ("{}.{} s", tenths / 10, tenths % 10);
}

std::string undoButtonText (const std::string& label)
{
    return label.empty() ? "Undo" : "Undo " + label;
}

std::string redoButtonText (const std::string& label)
{
    return label.empty() ? "Redo" : "Redo " + label;
}

std::string secondsText (song::SampleCount samples, int sampleRate)
{
    if (sampleRate <= 0)
        return "0.000 s";
    const auto sign = samples < 0 ? "-" : "";
    const auto magnitude = samples < 0 ? -samples : samples;
    const auto milliseconds = magnitude * 1000 / sampleRate;
    return std::format ("{}{}.{:03} s", sign, milliseconds / 1000, milliseconds % 1000);
}
} // namespace overdub::ui
