#include "ui/MixerGeometry.h"

#include "song/MixEdits.h"

#include <algorithm>
#include <cmath>
#include <format>

namespace overdub::ui
{
namespace
{
constexpr double lowerRangeDb = 0.0 - song::minVolumeDb; // 60 dB under unity
constexpr double upperRangeDb = song::maxVolumeDb;       // 12 dB over unity

double roundTo (double value, double step)
{
    return std::round (value / step) * step;
}
} // namespace

double faderPositionToDb (double position)
{
    const auto p = std::clamp (position, 0.0, 1.0);
    if (p <= faderUnityPosition)
        return song::minVolumeDb + lowerRangeDb * p / faderUnityPosition;
    return upperRangeDb * (p - faderUnityPosition) / (1.0 - faderUnityPosition);
}

double dbToFaderPosition (double db)
{
    const auto clamped = std::clamp (db, song::minVolumeDb, song::maxVolumeDb);
    if (clamped <= 0.0)
        return faderUnityPosition * (clamped - song::minVolumeDb) / lowerRangeDb;
    return faderUnityPosition + (1.0 - faderUnityPosition) * clamped / upperRangeDb;
}

double faderDragDb (double startDb, int deltaY, int travelPixels)
{
    if (travelPixels <= 0)
        return startDb;
    const auto position = dbToFaderPosition (startDb) - (double) deltaY / (double) travelPixels;
    return std::clamp (roundTo (faderPositionToDb (position), 0.1), song::minVolumeDb, song::maxVolumeDb);
}

double panDrag (double startPan, int deltaX, int widthPixels)
{
    if (widthPixels <= 0)
        return startPan;
    return std::clamp (roundTo (startPan + 2.0 * deltaX / widthPixels, 0.01), -1.0, 1.0);
}

double panPosition (double pan)
{
    return (std::clamp (pan, -1.0, 1.0) + 1.0) / 2.0;
}

double meterPosition (float peak)
{
    if (peak <= 0.0f)
        return 0.0;
    const auto db = 20.0 * std::log10 ((double) peak);
    if (db <= song::minVolumeDb)
        return 0.0;
    return dbToFaderPosition (db);
}

float nextMeterLevel (float shown, float peak)
{
    return std::max (peak, shown * meterFallPerPoll);
}

std::string panText (double pan)
{
    const auto percent = (int) std::lround (std::clamp (pan, -1.0, 1.0) * 100.0);
    if (percent == 0)
        return "C";
    return std::format ("{}{}", percent < 0 ? "L" : "R", std::abs (percent));
}

std::string volumeText (double db)
{
    const auto tenths = std::lround (db * 10.0);
    if (tenths == 0)
        return "0.0 dB";
    return std::format ("{}{:.1f} dB", tenths > 0 ? "+" : "", (double) tenths / 10.0);
}
} // namespace overdub::ui
