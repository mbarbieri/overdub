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
    if (status.recording)
        return std::format ("REC \xC2\xB7 PASS {}", status.passNumber);
    return status.playing ? "PLAY" : "STOP";
}

std::string undoButtonText (const std::string& label)
{
    return label.empty() ? "Undo" : "Undo " + label;
}

std::string redoButtonText (const std::string& label)
{
    return label.empty() ? "Redo" : "Redo " + label;
}
} // namespace overdub::ui
