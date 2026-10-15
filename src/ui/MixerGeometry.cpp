#include "ui/MixerGeometry.h"

#include "song/MixEdits.h"

#include <algorithm>
#include <cmath>
#include <format>
#include <optional>

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

// The drag travel that counts once the detent is taken out: `moved` pixels along the drag, with
// the neutral value `toNeutral` pixels from the start (same sign convention). nullopt: held at
// neutral. A drag that starts at neutral does not cross it: no detent.
std::optional<double> pastDetent (double moved, double toNeutral)
{
    if (toNeutral > 0.0 && moved >= toNeutral)
    {
        if (moved <= toNeutral + detentPixels)
            return std::nullopt;
        return moved - detentPixels;
    }
    if (toNeutral < 0.0 && moved <= toNeutral)
    {
        if (moved >= toNeutral - detentPixels)
            return std::nullopt;
        return moved + detentPixels;
    }
    return moved;
}

// One key step from `value`, stopping at 0 if it would cross it.
double stepTowards (double value, double step)
{
    const auto next = value + step;
    if ((value < 0.0 && next > 0.0) || (value > 0.0 && next < 0.0))
        return 0.0;
    return next;
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

double faderDragDb (double startDb, int louderPixels, int travelPixels)
{
    if (travelPixels <= 0)
        return startDb;
    const auto startPosition = dbToFaderPosition (startDb);
    const auto moved = pastDetent ((double) louderPixels, (faderUnityPosition - startPosition) * travelPixels);
    if (! moved.has_value())
        return 0.0;
    const auto position = startPosition + *moved / (double) travelPixels;
    return std::clamp (roundTo (faderPositionToDb (position), 0.1), song::minVolumeDb, song::maxVolumeDb);
}

double panDrag (double startPan, int deltaX, int widthPixels)
{
    if (widthPixels <= 0)
        return startPan;
    const auto moved = pastDetent ((double) deltaX, -startPan * widthPixels / 2.0);
    if (! moved.has_value())
        return 0.0;
    return std::clamp (roundTo (startPan + 2.0 * *moved / widthPixels, 0.01), -1.0, 1.0);
}

double faderKeyStepDb (double db, bool louder, bool fine)
{
    const auto step = (fine ? 0.1 : 1.0) * (louder ? 1.0 : -1.0);
    return std::clamp (roundTo (stepTowards (db, step), 0.1), song::minVolumeDb, song::maxVolumeDb);
}

double panKeyStep (double pan, bool right, bool fine)
{
    const auto step = (fine ? 0.01 : 0.05) * (right ? 1.0 : -1.0);
    return std::clamp (roundTo (stepTowards (pan, step), 0.01), -1.0, 1.0);
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
