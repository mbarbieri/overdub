#include "ui/CalibrationText.h"

#include <cmath>
#include <cstdio>

namespace overdub::ui
{
namespace
{
std::string oneDecimal (double value)
{
    char text[32];
    std::snprintf (text, sizeof (text), "%.1f", value);
    return text;
}

std::string milliseconds (song::SampleCount samples, int sampleRate)
{
    return oneDecimal (sampleRate > 0 ? 1000.0 * (double) samples / sampleRate : 0.0) + " ms";
}
} // namespace

std::string latencyCalibrationText (std::optional<song::SampleCount> storedOffset, int sampleRate)
{
    if (! storedOffset.has_value())
        return "Not calibrated: Takes are placed by the latency the device reports.";
    return "Calibrated: " + std::to_string (*storedOffset) + " samples (" + milliseconds (*storedOffset, sampleRate) + ").";
}

std::string latencyResultText (const capture::LoopbackMeasurement& measurement, int sampleRate)
{
    const auto jitter = std::to_string (measurement.jitterSamples) + " samples";
    if (! measurement.signalFound)
        return "No bursts came back. Check the cable from output 1 to input 1 and the input level, then measure again.";
    if (! capture::loopbackPassed (measurement))
        return "The bursts came back unevenly (jitter " + jitter + ", at most " + std::to_string (capture::loopbackJitterLimitSamples)
             + "). Use a cable rather than speakers, then measure again.";
    if (measurement.offsetSamples == 0)
        return "Takes land where they were heard (jitter " + jitter + "). Nothing to correct.";

    const auto size = std::abs (measurement.offsetSamples);
    return "Takes landed " + std::to_string (size) + " samples (" + milliseconds (size, sampleRate) + ") "
         + (measurement.offsetSamples > 0 ? "late" : "early") + ", jitter " + jitter + ". Use this calibration to correct it.";
}

std::string cameraCalibrationText (std::optional<double> storedOffsetMs)
{
    if (! storedOffsetMs.has_value())
        return "Not calibrated: 0 ms.";
    return "Calibrated: " + oneDecimal (*storedOffsetMs) + " ms (video " + (*storedOffsetMs >= 0.0 ? "late" : "early") + ").";
}

std::string clapResultText (const capture::ClapReport& report)
{
    if (! report.meanOffsetMs.has_value())
        return "No clap was seen. Record a Pass clapping 3 to 5 times in frame, then measure again.";

    std::size_t seen = 0;
    for (const auto& clap : report.claps)
        if (clap.offsetMs.has_value())
            ++seen;
    const auto mean = *report.meanOffsetMs;
    return std::to_string (seen) + " of " + std::to_string (report.claps.size()) + " claps seen: the video was " + oneDecimal (std::abs (mean)) + " ms "
         + (mean >= 0.0 ? "late" : "early") + ". Use this calibration to correct it.";
}
} // namespace overdub::ui
